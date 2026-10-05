// The decoded-frame buffer the FP16 NCHW bridge binds, and the one canonical
// packer that turns it into the bytes both sides of a verification consume.
//
// This header deliberately carries no ONNX Runtime type and no ORT include: it
// belongs to the unconditional native support library, so the reference
// exporter (built only together with the model inspector), the OFF-feature
// build and the test suite all reach one declaration and one implementation
// instead of a second copy or a link-time hole.

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QtGlobal>

namespace mplayer {
namespace native {

/// One decoded frame as the packing contract needs it: the geometry, the raw
/// interleaved RGB8 payload, and the provenance of the file it was read from.
struct FrameFixture {
  QSize size;
  QByteArray rgb;
  QString path;
  qint64 fileBytes = 0;
  QString fileSha256;
};

/// The one canonical decoded-frame packer: planar NCHW, channel-major,
/// little-endian fp16, every sample divided by 255, exactly the layout the FP16
/// bridge binds. Pure byte math with no ORT and no model dependency, so its
/// implementation is unconditional (`NativeFrameFixture.cpp`): the reference
/// exporter, the OFF-feature build and the test suite share this packer
/// instead of each carrying a copy.
QByteArray frameToFp16Tensor(const FrameFixture& fixture);

}  // namespace native
}  // namespace mplayer
