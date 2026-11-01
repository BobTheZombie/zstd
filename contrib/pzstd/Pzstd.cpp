/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */
#include "platform.h"   /* Large Files support, SET_BINARY_MODE */
#include "Pzstd.h"
#include "SkippableFrame.h"
#include "mem.h"
#include "util.h"
#include "utils/FileSystem.h"
#include "utils/Portability.h"
#include "utils/Range.h"
#include "utils/ScopeGuard.h"
#include "utils/ThreadPool.h"
#include "utils/WorkQueue.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>


namespace pzstd {

namespace {
#ifdef _WIN32
const std::string nullOutput = "nul";
#else
const std::string nullOutput = "/dev/null";
#endif
}

using std::size_t;

static constexpr size_t kParallelIOSize = size_t{1} << 20;

static std::uintmax_t fileSizeOrZero(const std::string &file) {
  if (file == "-") {
    return 0;
  }
  std::error_code ec;
  auto size = file_size(file, ec);
  if (ec) {
    size = 0;
  }
  return size;
}

static std::uint64_t handleOneInput(const Options &options,
                             const std::string &inputFile,
                             FILE* inputFd,
                             const std::string &outputFile,
                             FILE* outputFd,
                             SharedState& state) {
  auto inputSize = fileSizeOrZero(inputFile);
  // WorkQueue outlives ThreadPool so in the case of error we are certain
  // we don't accidentally try to call push() on it after it is destroyed
  WorkQueue<std::shared_ptr<BufferWorkQueue>> outs{options.numThreads + 1};
  std::uint64_t bytesRead;
  std::uint64_t bytesWritten;
  {
    // Initialize the (de)compression thread pool with numThreads
    ThreadPool executor(options.numThreads);
    // Run the reader thread on an extra thread
    ThreadPool readExecutor(1);
    if (!options.decompress) {
      // Add a job that reads the input and starts all the compression jobs
      readExecutor.add(
          [&state, &outs, &executor, inputFd, inputSize, &options, &bytesRead] {
            bytesRead = asyncCompressChunks(
                state,
                outs,
                executor,
                inputFd,
                inputSize,
                options.numThreads,
                options.determineParameters());
          });
      // Start writing
      bytesWritten = writeFile(state, outs, outputFd, options.decompress);
    } else {
      // Add a job that reads the input and starts all the decompression jobs
      readExecutor.add([&state, &outs, &executor, inputFd, &bytesRead] {
        bytesRead = asyncDecompressFrames(state, outs, executor, inputFd);
      });
      // Start writing
      bytesWritten = writeFile(state, outs, outputFd, options.decompress);
    }
  }
  if (!state.errorHolder.hasError()) {
    std::string inputFileName = inputFile == "-" ? "stdin" : inputFile;
    std::string outputFileName = outputFile == "-" ? "stdout" : outputFile;
    if (!options.decompress) {
      double ratio = static_cast<double>(bytesWritten) /
                     static_cast<double>(bytesRead + !bytesRead);
      state.log(kLogInfo, "%-20s :%6.2f%%   (%6" PRIu64 " => %6" PRIu64
                   " bytes, %s)\n",
                   inputFileName.c_str(), ratio * 100, bytesRead, bytesWritten,
                   outputFileName.c_str());
    } else {
      state.log(kLogInfo, "%-20s: %" PRIu64 " bytes \n",
                   inputFileName.c_str(),bytesWritten);
    }
  }
  return bytesWritten;
}

static FILE *openInputFile(const std::string &inputFile,
                           ErrorHolder &errorHolder) {
  if (inputFile == "-") {
    SET_BINARY_MODE(stdin);
    return stdin;
  }
  // Check if input file is a directory
  {
    std::error_code ec;
    if (is_directory(inputFile, ec)) {
      errorHolder.setError("Output file is a directory -- ignored");
      return nullptr;
    }
  }
  auto inputFd = std::fopen(inputFile.c_str(), "rb");
  if (!errorHolder.check(inputFd != nullptr, "Failed to open input file")) {
    return nullptr;
  }
  return inputFd;
}

static FILE *openOutputFile(const Options &options,
                            const std::string &outputFile,
                            SharedState& state) {
  if (outputFile == "-") {
    SET_BINARY_MODE(stdout);
    return stdout;
  }
  // Check if the output file exists and then open it
  if (!options.overwrite && outputFile != nullOutput) {
    auto outputFd = std::fopen(outputFile.c_str(), "rb");
    if (outputFd != nullptr) {
      std::fclose(outputFd);
      if (!state.log.logsAt(kLogInfo)) {
        state.errorHolder.setError("Output file exists");
        return nullptr;
      }
      state.log(
          kLogInfo,
          "pzstd: %s already exists; do you wish to overwrite (y/n) ? ",
          outputFile.c_str());
      int c = getchar();
      if (c != 'y' && c != 'Y') {
        state.errorHolder.setError("Not overwritten");
        return nullptr;
      }
    }
  }
  auto outputFd = std::fopen(outputFile.c_str(), "wb");
  if (!state.errorHolder.check(
          outputFd != nullptr, "Failed to open output file")) {
    return nullptr;
  }
  return outputFd;
}

int pzstdMain(const Options &options) {
  int returnCode = 0;
  SharedState state(options);
  for (const auto& input : options.inputFiles) {
    // Setup the shared state
    auto printErrorGuard = makeScopeGuard([&] {
      if (state.errorHolder.hasError()) {
        returnCode = 1;
        state.log(kLogError, "pzstd: %s: %s.\n", input.c_str(),
                  state.errorHolder.getError().c_str());
      }
    });
    // Open the input file
    auto inputFd = openInputFile(input, state.errorHolder);
    if (inputFd == nullptr) {
      continue;
    }
    auto closeInputGuard = makeScopeGuard([&] { std::fclose(inputFd); });
    // Open the output file
    auto outputFile = options.getOutputFile(input);
    if (!state.errorHolder.check(outputFile != "",
                           "Input file does not have extension .zst")) {
      continue;
    }
    if (input != "-" && outputFile != "-" &&
        !state.errorHolder.check(!UTIL_isSameFile(input.c_str(), outputFile.c_str()),
                                "Input and output refer to the same file")) {
      continue;
    }
    auto outputFd = openOutputFile(options, outputFile, state);
    if (outputFd == nullptr) {
      continue;
    }
    auto closeOutputGuard = makeScopeGuard([&] {
      if (outputFd != stdout) std::fclose(outputFd);
    });
    // (de)compress the file
    handleOneInput(options, input, inputFd, outputFile, outputFd, state);
    // Buffered writes may fail only at flush/close. Never report success or
    // remove a source before this result has been checked.
    const int outputStatus = outputFd == stdout ? std::fflush(outputFd)
                                                : std::fclose(outputFd);
    closeOutputGuard.dismiss();
    state.errorHolder.check(outputStatus == 0, "Failed to close or flush output file");
    if (state.errorHolder.hasError()) {
      continue;
    }
    // Delete the input file if necessary
    if (!options.keepSource) {
      // Be sure that we are done and have written everything before we delete
      const int inputStatus = std::fclose(inputFd);
      closeInputGuard.dismiss();
      if (!state.errorHolder.check(inputStatus == 0, "Failed to close input file")) {
        continue;
      }
      if (std::remove(input.c_str()) != 0) {
        state.errorHolder.setError("Failed to remove input file");
        continue;
      }
    }
  }
  // Returns 1 if any of the files failed to (de)compress.
  return returnCode;
}

/// Construct a `ZSTD_inBuffer` that points to the data in `buffer`.
static ZSTD_inBuffer makeZstdInBuffer(const Buffer& buffer) {
  return ZSTD_inBuffer{buffer.data(), buffer.size(), 0};
}

/**
 * Advance `buffer` and `inBuffer` by the amount of data read, as indicated by
 * `inBuffer.pos`.
 */
void advance(Buffer& buffer, ZSTD_inBuffer& inBuffer) {
  auto pos = inBuffer.pos;
  inBuffer.src = static_cast<const unsigned char*>(inBuffer.src) + pos;
  inBuffer.size -= pos;
  inBuffer.pos = 0;
  return buffer.advance(pos);
}

/// Construct a `ZSTD_outBuffer` that points to the data in `buffer`.
static ZSTD_outBuffer makeZstdOutBuffer(Buffer& buffer) {
  return ZSTD_outBuffer{buffer.data(), buffer.size(), 0};
}

/**
 * Split `buffer` and advance `outBuffer` by the amount of data written, as
 * indicated by `outBuffer.pos`.
 */
Buffer split(Buffer& buffer, ZSTD_outBuffer& outBuffer) {
  auto pos = outBuffer.pos;
  outBuffer.dst = static_cast<unsigned char*>(outBuffer.dst) + pos;
  outBuffer.size -= pos;
  outBuffer.pos = 0;
  return buffer.splitAt(pos);
}

/**
 * Stream chunks of input from `in`, compress it, and stream it out to `out`.
 *
 * @param state        The shared state
 * @param in           Queue that we `pop()` input buffers from
 * @param out          Queue that we `push()` compressed output buffers to
 * @param maxInputSize An upper bound on the size of the input
 */
static void compress(
    SharedState& state,
    std::shared_ptr<BufferWorkQueue> in,
    std::shared_ptr<BufferWorkQueue> out,
    size_t maxInputSize) {
  auto& errorHolder = state.errorHolder;
  auto guard = makeScopeGuard([&] {
    in->finish();
    out->finish();
  });
  // Initialize the CCtx
  auto ctx = state.cStreamPool->get();
  if (!errorHolder.check(ctx != nullptr, "Failed to allocate ZSTD_CStream")) {
    return;
  }
  {
    auto err = ZSTD_CCtx_reset(ctx.get(), ZSTD_reset_session_only);
    if (!errorHolder.check(!ZSTD_isError(err), ZSTD_getErrorName(err))) {
      return;
    }
  }

  // Allocate space for the result
  auto outBuffer = Buffer(ZSTD_compressBound(maxInputSize));
  auto zstdOutBuffer = makeZstdOutBuffer(outBuffer);
  {
    Buffer inBuffer;
    // Read a buffer in from the input queue
    while (in->pop(inBuffer) && !errorHolder.hasError()) {
      auto zstdInBuffer = makeZstdInBuffer(inBuffer);
      // Compress the whole buffer and send it to the output queue
      while (!inBuffer.empty() && !errorHolder.hasError()) {
        if (!errorHolder.check(
                !outBuffer.empty(), "ZSTD_compressBound() was too small")) {
          return;
        }
        // Compress
        auto err =
            ZSTD_compressStream(ctx.get(), &zstdOutBuffer, &zstdInBuffer);
        if (!errorHolder.check(!ZSTD_isError(err), ZSTD_getErrorName(err))) {
          return;
        }
        // Split the compressed data off outBuffer and pass to the output queue
        auto output = split(outBuffer, zstdOutBuffer);
        if (!output.empty()) out->push(std::move(output));
        // Forget about the data we already compressed
        advance(inBuffer, zstdInBuffer);
      }
    }
  }
  // Write the epilog
  size_t bytesLeft;
  do {
    if (!errorHolder.check(
            !outBuffer.empty(), "ZSTD_compressBound() was too small")) {
      return;
    }
    bytesLeft = ZSTD_endStream(ctx.get(), &zstdOutBuffer);
    if (!errorHolder.check(
            !ZSTD_isError(bytesLeft), ZSTD_getErrorName(bytesLeft))) {
      return;
    }
    auto output = split(outBuffer, zstdOutBuffer);
    if (!output.empty()) out->push(std::move(output));
  } while (bytesLeft != 0 && !errorHolder.hasError());
}

/**
 * Calculates how large each independently compressed frame should be.
 *
 * @param size       The size of the source if known, 0 otherwise
 * @param numThreads The number of threads available to run compression jobs on
 * @param params     The zstd parameters to be used for compression
 */
static size_t calculateStep(
    std::uintmax_t size,
    size_t numThreads,
    const ZSTD_parameters &params) {

  // Not validated to work correctly for window logs > 23.
  // It will definitely fail if windowLog + 2 is >= 4GB because
  // the skippable frame can only store sizes up to 4GB.
  assert(params.cParams.windowLog <= 23);
  size_t step = size_t{1} << (params.cParams.windowLog + 2);
  // Give medium-sized regular files enough independent frames to occupy
  // the workers. Preserve the old cap and avoid sub-MiB frame overhead.
  if (size != 0 && numThreads > 1) {
    const auto target = size / numThreads + (size % numThreads != 0);
    while (step > (size_t{1} << 20) && step > target) {
      step /= 2;
    }
  }
  return step;
}

namespace {
enum class FileStatus { Continue, Done, Error };
/// Determines the status of the file descriptor `fd`.
FileStatus fileStatus(FILE* fd) {
  if (std::feof(fd)) {
    return FileStatus::Done;
  } else if (std::ferror(fd)) {
    return FileStatus::Error;
  }
  return FileStatus::Continue;
}
} // anonymous namespace

/**
 * Reads `size` data in chunks of `chunkSize` and puts it into `queue`.
 * Will read less if an error or EOF occurs.
 * Returns the status of the file after all of the reads have occurred.
 */
static FileStatus
readData(BufferWorkQueue& queue, size_t chunkSize, size_t size, FILE* fd,
         std::uint64_t *totalBytesRead) {
  Buffer buffer(size);
  while (!buffer.empty()) {
    auto bytesRead =
        std::fread(buffer.data(), 1, std::min(chunkSize, buffer.size()), fd);
    *totalBytesRead += bytesRead;
    queue.push(buffer.splitAt(bytesRead));
    auto status = fileStatus(fd);
    if (status != FileStatus::Continue) {
      return status;
    }
  }
  return FileStatus::Continue;
}

std::uint64_t asyncCompressChunks(
    SharedState& state,
    WorkQueue<std::shared_ptr<BufferWorkQueue>>& chunks,
    ThreadPool& executor,
    FILE* fd,
    std::uintmax_t size,
    size_t numThreads,
    ZSTD_parameters params) {
  auto chunksGuard = makeScopeGuard([&] { chunks.finish(); });
  std::uint64_t bytesRead = 0;

  // Break the input up into chunks of size `step` and compress each chunk
  // independently.
  size_t step = calculateStep(size, numThreads, params);
  state.log(kLogDebug, "Chosen frame size: %zu\n", step);
  auto status = FileStatus::Continue;
  while (status == FileStatus::Continue && !state.errorHolder.hasError()) {
    // Make a new input queue that we will put the chunk's input data into.
    auto in = std::make_shared<BufferWorkQueue>();
    auto inGuard = makeScopeGuard([&] { in->finish(); });
    // Make a new output queue that compress will put the compressed data into.
    auto out = std::make_shared<BufferWorkQueue>();
    // Start compression in the thread pool
    executor.add([&state, in, out, step] {
      return compress(
          state, std::move(in), std::move(out), step);
    });
    // Pass the output queue to the writer thread.
    chunks.push(std::move(out));
    state.log(kLogVerbose, "%s\n", "Starting a new frame");
    // Fill the input queue for the compression job we just started
    status = readData(*in, std::max(ZSTD_CStreamInSize(), kParallelIOSize),
                      step, fd, &bytesRead);
  }
  state.errorHolder.check(status != FileStatus::Error, "Error reading input");
  return bytesRead;
}

/**
 * Decompress a frame, whose data is streamed into `in`, and stream the output
 * to `out`.
 *
 * @param state        The shared state
 * @param in           Queue that we `pop()` input buffers from. It contains
 *                      exactly one compressed frame.
 * @param out          Queue that we `push()` decompressed output buffers to
 */
static void decompress(
    SharedState& state,
    std::shared_ptr<BufferWorkQueue> in,
    std::shared_ptr<BufferWorkQueue> out) {
  auto& errorHolder = state.errorHolder;
  auto guard = makeScopeGuard([&] {
    in->finish();
    out->finish();
  });
  // Initialize the DCtx
  auto ctx = state.dStreamPool->get();
  if (!errorHolder.check(ctx != nullptr, "Failed to allocate ZSTD_DStream")) {
    return;
  }
  {
    auto err = ZSTD_DCtx_reset(ctx.get(), ZSTD_reset_session_only);
    if (!errorHolder.check(!ZSTD_isError(err), ZSTD_getErrorName(err))) {
      return;
    }
  }

  const size_t outSize = std::max(ZSTD_DStreamOutSize(), kParallelIOSize);
  Buffer inBuffer;
  size_t returnCode = 0;
  // Read a buffer in from the input queue
  while (in->pop(inBuffer) && !errorHolder.hasError()) {
    auto zstdInBuffer = makeZstdInBuffer(inBuffer);
    // Decompress the whole buffer and send it to the output queue
    while (!inBuffer.empty() && !errorHolder.hasError()) {
      // Allocate a buffer with at least outSize bytes.
      Buffer outBuffer(outSize);
      auto zstdOutBuffer = makeZstdOutBuffer(outBuffer);
      // Decompress
      returnCode =
          ZSTD_decompressStream(ctx.get(), &zstdOutBuffer, &zstdInBuffer);
      if (!errorHolder.check(
              !ZSTD_isError(returnCode), ZSTD_getErrorName(returnCode))) {
        return;
      }
      // Pass the buffer with the decompressed data to the output queue
      auto output = split(outBuffer, zstdOutBuffer);
      if (!output.empty()) out->push(std::move(output));
      // Advance past the input we already read
      advance(inBuffer, zstdInBuffer);
      if (returnCode == 0) {
        // The frame is over, prepare to (maybe) start a new frame
        ZSTD_initDStream(ctx.get());
      }
    }
  }
  if (!errorHolder.check(returnCode <= 1, "Incomplete block")) {
    return;
  }
  // We've given ZSTD_decompressStream all of our data, but there may still
  // be data to read.
  while (returnCode == 1) {
    // Allocate a buffer with at least outSize bytes.
    Buffer outBuffer(outSize);
    auto zstdOutBuffer = makeZstdOutBuffer(outBuffer);
    // Pass in no input.
    ZSTD_inBuffer zstdInBuffer{nullptr, 0, 0};
    // Decompress
    returnCode =
        ZSTD_decompressStream(ctx.get(), &zstdOutBuffer, &zstdInBuffer);
    if (!errorHolder.check(
            !ZSTD_isError(returnCode), ZSTD_getErrorName(returnCode))) {
      return;
    }
    if (!errorHolder.check(returnCode == 0 || zstdOutBuffer.pos != 0,
                           "Incomplete frame")) {
      return;
    }
    // Pass the buffer with the decompressed data to the output queue
    auto output = split(outBuffer, zstdOutBuffer);
    if (!output.empty()) out->push(std::move(output));
  }
}

namespace {
// Read a bounded piece, distinguishing an I/O error from a truncated frame.
bool readExact(SharedState& state, FILE* fd, void* dst, size_t size,
               std::uint64_t& total) {
  const auto count = std::fread(dst, 1, size, fd);
  total += count;
  return state.errorHolder.check(count == size,
      std::ferror(fd) ? "Error reading input" : "Truncated frame");
}

bool skipBytes(SharedState& state, FILE* fd, size_t size, std::uint64_t& total) {
  Buffer data(std::min(size, ZSTD_DStreamInSize()));
  while (size != 0 && !state.errorHolder.hasError()) {
    const auto count = std::min(size, ZSTD_DStreamInSize());
    if (!readExact(state, fd, data.data(), count, total)) {
      return false;
    }
    size -= count;
  }
  return !state.errorHolder.hasError();
}
// Batch headers and payloads into large queue messages. Small block-header
// messages otherwise dominate fast decoding with allocations and wakeups.
class FrameInput {
  BufferWorkQueue& queue_;
  Buffer buffer_{kParallelIOSize};
  size_t used_ = 0;

 public:
  explicit FrameInput(BufferWorkQueue& queue) : queue_(queue) {}

  void flush(bool more = true) {
    if (used_ != 0) {
      buffer_.subtract(buffer_.size() - used_);
      queue_.push(std::move(buffer_));
      buffer_ = more ? Buffer(kParallelIOSize) : Buffer();
      used_ = 0;
    }
  }

  void append(const void* source, size_t size) {
    auto bytes = static_cast<const unsigned char*>(source);
    while (size != 0) {
      const auto count = std::min(size, buffer_.size() - used_);
      std::copy(bytes, bytes + count, buffer_.data() + used_);
      used_ += count;
      bytes += count;
      size -= count;
      if (used_ == buffer_.size()) flush();
    }
  }

  bool read(SharedState& state, FILE* fd, size_t size, std::uint64_t& total) {
    while (size != 0 && !state.errorHolder.hasError()) {
      const auto count = std::min(size, buffer_.size() - used_);
      if (!readExact(state, fd, buffer_.data() + used_, count, total)) return false;
      used_ += count;
      size -= count;
      if (used_ == buffer_.size()) flush();
    }
    return !state.errorHolder.hasError();
  }
};
} // anonymous namespace

std::uint64_t asyncDecompressFrames(
    SharedState& state,
    WorkQueue<std::shared_ptr<BufferWorkQueue>>& frames,
    ThreadPool& executor,
    FILE* fd) {
  auto framesGuard = makeScopeGuard([&] { frames.finish(); });
  std::uint64_t totalBytesRead = 0;
  bool sawFrame = false;

  // Delimit actual standard frames by their block headers. This works for
  // ordinary concatenated .zst streams, unknown content sizes, and pipes;
  // pzstd's optional skippable size hints are no longer trusted as indexes.
  while (!state.errorHolder.hasError()) {
    unsigned char prefix[4];
    const auto count = std::fread(prefix, 1, sizeof(prefix), fd);
    totalBytesRead += count;
    if (count == 0 && std::feof(fd)) {
      state.errorHolder.check(sawFrame, "Empty compressed input");
      break;
    }
    if (!state.errorHolder.check(count == sizeof(prefix),
          std::ferror(fd) ? "Error reading input" : "Truncated frame magic")) {
      break;
    }
    const auto magic = MEM_readLE32(prefix);
    if ((magic & 0xfffffff0U) == ZSTD_MAGIC_SKIPPABLE_START) {
      unsigned char length[4];
      if (!readExact(state, fd, length, sizeof(length), totalBytesRead) ||
          !skipBytes(state, fd, MEM_readLE32(length), totalBytesRead)) {
        break;
      }
      sawFrame = true;
      continue;
    }

    // Bounded queues provide backpressure even for huge or unknown-size
    // frames. Only the writer emits output, preserving frame order.
    auto in = std::make_shared<BufferWorkQueue>(4);
    auto out = std::make_shared<BufferWorkQueue>(4);
    auto inGuard = makeScopeGuard([&] { in->finish(); });
    executor.add([&state, in, out] {
      decompress(state, in, out);
    });
    frames.push(std::move(out));
    sawFrame = true;

    if (magic != ZSTD_MAGICNUMBER) {
      // Preserve the legacy decoder path without inventing boundaries for
      // formats not described by the current Zstandard frame specification.
      Buffer first(sizeof(prefix));
      std::copy(prefix, prefix + sizeof(prefix), first.data());
      in->push(std::move(first));
      auto status = FileStatus::Continue;
      while (status == FileStatus::Continue && !state.errorHolder.hasError()) {
        status = readData(*in, ZSTD_DStreamInSize(), ZSTD_DStreamInSize(),
                          fd, &totalBytesRead);
      }
      state.errorHolder.check(status != FileStatus::Error, "Error reading input");
      break;
    }

    Buffer header(ZSTD_FRAMEHEADERSIZE_MAX);
    std::copy(prefix, prefix + sizeof(prefix), header.data());
    size_t headerSize = sizeof(prefix);
    ZSTD_frameHeader frameHeader;
    while (!state.errorHolder.hasError()) {
      const auto needed = ZSTD_getFrameHeader(&frameHeader, header.data(), headerSize);
      if (!state.errorHolder.check(!ZSTD_isError(needed), ZSTD_getErrorName(needed))) {
        break;
      }
      if (needed == 0) {
        break;
      }
      if (!state.errorHolder.check(needed > headerSize && needed <= header.size(),
                                  "Invalid frame header size") ||
          !readExact(state, fd, header.data() + headerSize,
                     needed - headerSize, totalBytesRead)) {
        break;
      }
      headerSize = needed;
    }
    if (state.errorHolder.hasError()) {
      break;
    }
    FrameInput input(*in);
    input.append(header.data(), headerSize);
    state.log(kLogVerbose, "%s\n", "Decompressing an independent frame");

    bool last = false;
    do {
      unsigned char block[3];
      if (!readExact(state, fd, block, sizeof(block), totalBytesRead)) {
        break;
      }
      const auto bits = static_cast<unsigned>(block[0]) |
          (static_cast<unsigned>(block[1]) << 8) |
          (static_cast<unsigned>(block[2]) << 16);
      last = (bits & 1) != 0;
      const auto type = (bits >> 1) & 3;
      const auto size = bits >> 3;
      if (!state.errorHolder.check(type != 3 && size <= frameHeader.blockSizeMax,
                                  "Invalid block header")) {
        break;
      }
      input.append(block, sizeof(block));
      if (!input.read(state, fd, type == 1 ? 1 : size, totalBytesRead)) {
        break;
      }
    } while (!last && !state.errorHolder.hasError());
    if (!state.errorHolder.hasError() && frameHeader.checksumFlag) {
      input.read(state, fd, 4, totalBytesRead);
    }
    input.flush(false);
  }
  return totalBytesRead;
}

/// Write `data` to `fd`, returns true iff success.
static bool writeData(ByteRange data, FILE* fd) {
  while (!data.empty()) {
    data.advance(std::fwrite(data.begin(), 1, data.size(), fd));
    if (std::ferror(fd)) {
      return false;
    }
  }
  return true;
}

std::uint64_t writeFile(
    SharedState& state,
    WorkQueue<std::shared_ptr<BufferWorkQueue>>& outs,
    FILE* outputFd,
    bool decompress) {
  auto& errorHolder = state.errorHolder;
  auto outsFinishGuard = makeScopeGuard([&outs] { outs.finish(); });
  auto lineClearGuard = makeScopeGuard([&state] {
    state.log.clear(kLogInfo);
  });
  std::uint64_t bytesWritten = 0;
  std::shared_ptr<BufferWorkQueue> out;
  // Grab the output queue for each decompression job (in order).
  while (outs.pop(out)) {
    auto outFinishGuard = makeScopeGuard([&out] { out->finish(); });
    if (errorHolder.hasError()) {
      continue;
    }
    if (!decompress) {
      // If we are compressing and want to write skippable frames we can't
      // start writing before compression is done because we need to know the
      // compressed size.
      // Wait for the compressed size to be available and write skippable frame
      assert(uint64_t(out->size()) < uint64_t(1) << 32);
      SkippableFrame frame(uint32_t(out->size()));
      if (!writeData(frame.data(), outputFd)) {
        errorHolder.setError("Failed to write output");
        return bytesWritten;
      }
      bytesWritten += frame.kSize;
    }
    // For each chunk of the frame: Pop it from the queue and write it
    Buffer buffer;
    while (out->pop(buffer) && !errorHolder.hasError()) {
      if (!writeData(buffer.range(), outputFd)) {
        errorHolder.setError("Failed to write output");
        return bytesWritten;
      }
      bytesWritten += buffer.size();
      state.log.update(kLogInfo, "Written: %u MB   ",
                static_cast<std::uint32_t>(bytesWritten >> 20));
    }
  }
  return bytesWritten;
}
}
