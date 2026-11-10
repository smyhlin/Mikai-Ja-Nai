// The decoded-frame side of the FP16 NCHW bridge, compiled unconditionally.
//
// This unit is deliberately free of ONNX Runtime, of the model inspector and of
// any model dependency: it is the one canonical implementation of the packing
// contract, so the reference exporter (built only when the model inspector is
// built) and the test suite (always built) link the same function instead of
// each carrying a copy or one of them hitting a link-time hole.

#include "NativeFrameFixture.hpp"

#include "NativeBuilderInternal.hpp"

#include <QtCore/QtEndian>

#include <cstring>

namespace mplayer {
namespace native {

namespace {


}  // namespace

/**
 * The exact layout the FP16 bridge binds, and the only one it binds: planar
 * NCHW, channel-major, little-endian fp16, every sample divided by 255 into
 * [0, 1]. The tensor is consumed twice by construction - once by the CPU
 * reference run and once by the engine bytes that are compared against it - and
 * both bind contiguous NCHW blocks, so an interleaved buffer written where the
 * planes belong is not a cosmetic slip: it feeds both sides a scrambled image
 * and the comparison silently measures a frame nobody decoded. Asserted by
 * TestNativeAssets::rgbFrameComponentsArePreservedInNchwFp16Planes.
 */
QByteArray frameToFp16Tensor(const FrameFixture& fixture) {
  const qint64 planePixels =
      static_cast<qint64>(fixture.size.width()) * fixture.size.height();
  const qint64 elements = planePixels * 3;
  QByteArray tensor(static_cast<qsizetype>(elements) * 2, '\0');
  const auto* in = reinterpret_cast<const quint8*>(fixture.rgb.constData());
  for (qint64 channel = 0; channel < 3; ++channel) {
    for (qint64 pixel = 0; pixel < planePixels; ++pixel) {
      const qint64 sourceIndex = pixel * 3 + channel;
      const qint64 tensorIndex = channel * planePixels + pixel;
      const quint16 half =
          builder_detail::floatToHalf(static_cast<float>(in[sourceIndex]) / 255.0f);
      qToLittleEndian<quint16>(half, tensor.data() + tensorIndex * 2);
    }
  }
  return tensor;
}

}  // namespace native
}  // namespace mplayer
