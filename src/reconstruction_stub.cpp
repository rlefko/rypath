// SPDX-License-Identifier: GPL-3.0-only
#include "reconstruction.hpp"
namespace rypath {
struct ReconstructedFrame::Impl {};
struct ReconstructionStream::Impl {};
ReconstructedFrame::ReconstructedFrame(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
ReconstructedFrame::~ReconstructedFrame() = default;
ReconstructedFrame::ReconstructedFrame(ReconstructedFrame &&) noexcept = default;
ReconstructedFrame &ReconstructedFrame::operator=(ReconstructedFrame &&) noexcept = default;
ReconstructionInfo ReconstructedFrame::poll() const {
    return {.status = ReconstructionStatus::failed,
            .diagnostic = "Metal reconstruction was not built"};
}
ReconstructionInfo ReconstructedFrame::wait() const { return poll(); }
ReconstructionCapture ReconstructedFrame::capture() const {
    return {.status = GeometryStatus::not_built,
            .diagnostic = "Metal reconstruction was not built"};
}
ReconstructionStream::ReconstructionStream(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
ReconstructionStream::~ReconstructionStream() = default;
ReconstructionStream::ReconstructionStream(ReconstructionStream &&) noexcept = default;
ReconstructionStream &ReconstructionStream::operator=(ReconstructionStream &&) noexcept = default;
ReconstructionSubmitResult ReconstructionStream::submit(const Frame &, bool) {
    return {.status = GeometryStatus::not_built,
            .diagnostic = "Metal reconstruction was not built"};
}
ReconstructionBuildResult build_reconstruction_stream(const Frame &, const ReconstructionConfig &) {
    return {.status = GeometryStatus::not_built,
            .diagnostic = "Metal reconstruction was not built"};
}
} // namespace rypath
