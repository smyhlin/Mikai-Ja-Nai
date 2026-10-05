// The independently observed facts a preparation caller must hold before it may
// ask for a plan to be prepared. Every member is what somebody actually observed
// for the live source and device; an absent fact stays absent, never a guessed
// UUID, cadence or module.

#pragma once

#include <optional>

#include <QtCore/QMetaType>
#include <QtCore/QString>

#include "NativePreparationPlan.hpp"
#include "NativeStatus.hpp"
#include "NativeTypes.hpp"

namespace mplayer {
namespace native {

struct NativePreparationContext {
  /// The decoder's own report for the live source. A caller plans from this and
  /// nothing else; a member the producer did not observe stays absent.
  SourceGeometry source;
  /// The device the producer observed. Never copied from a decoder UUID read
  /// elsewhere: the compute side is the producer's independent query.
  DeviceIdentity device;
  /// The mpv module the live core was actually loaded from, absolute and
  /// canonical. The preparation worker measures the toolchain itself; this names
  /// the runtime the prepared artifacts must run under.
  QString modulePath;
  /// The producer's OPTIONAL factory observation for this same source, taken
  /// from the one status document that carried `source`/`device` above. Absent
  /// stays absent: a caller that wants eligibility must supply this to the
  /// availability producer, and a context that never observed a factory is not
  /// evidence that one exists. It is an availability fact only, never a
  /// prepared or running plan.
  std::optional<NativeFactoryObservation> factory;
  /// The decoder the core was actually aligned to for this observation, exactly
  /// as the core reported it: "nvdec", "d3d11", "no" for software decoding, or
  /// any other hwdec method this build can name. Empty when no alignment has been
  /// observed. It is never inferred from the requested backend, from a factory,
  /// or from the stored preference: frames one decoder produced say nothing about
  /// another.
  QString decoder;
  /// The backend the alignment above was requested for, so a caller can see WHICH
  /// request this observation answers. Absent when no alignment was asked for.
  std::optional<Backend> desiredBackend;
};

}  // namespace native
}  // namespace mplayer

// Declared where the type is defined, so any header that uses it in a signal -
// and the moc code generated for that header - sees this before QMetaTypeId is
// instantiated for it. Declaring it in a consumer header would be too late.
Q_DECLARE_METATYPE(mplayer::native::NativePreparationContext)
