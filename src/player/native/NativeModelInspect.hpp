// Reading what an ONNX graph actually declares, and resolving the geometry it
// actually produces at a stage's source size.
//
// The declaration is read through the *official* ONNX Runtime C API - the same
// runtime the DirectML/TensorRT staging is expected to use later - by loading an
// explicitly named `onnxruntime.dll` at an absolute path with restricted search.
// Nothing here parses the protobuf by hand, nothing here reads a file name to
// decide what a graph is, and nothing here invents a dtype, a rank, a geometry or
// a scale that the graph did not state or the CPU run did not produce.
//
// This header deliberately carries **no ONNX Runtime type and no ORT include**.
// The Qt build and the GUI have no native SDK dependency; only the translation
// unit that implements the inspection includes the ORT C API, and that unit is
// built into its own target. A machine without the ORT package still configures
// and builds everything else, and every call here answers
// `inspect.runtime.unavailable` with a named reason rather than silently
// succeeding against some other DLL.
//
// The dtype boundary is the FP16 NCHW bridge's, not a suggestion: FP32 and BF16
// graphs are recorded as unsupported with the observed dtype named, never
// converted, never re-exported and never promoted. Only an FP16 graph on both
// tensors is ever executed.

#pragma once

#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtCore/QString>

#include "NativeCatalog.hpp"
#include "NativeTypes.hpp"

namespace mplayer {
namespace native {

/// Absolute location of an ONNX Runtime an inspector may load.
///
/// The caller names the directory; the process PATH, the current directory and
/// the default Windows folders are never searched for it. A runtime that is not
/// where it was named is an unavailability with a reason, which is the honest
/// answer and the one the CLI reports.
struct InspectionRuntime {
  QString directory;
  /// `directory/onnxruntime.dll`, filled by `inspectionRuntimeAt`.
  QString modulePath;
  /// The version string the loaded module reports (empty until a successful
  /// load). Never a version assumed from the package name.
  QString version;

  bool isSet() const { return !directory.isEmpty(); }
};

/// "onnxruntime.dll". The one module this track loads.
QString inspectionRuntimeModuleName();

/// Builds a location record for `directory` - absolute, or empty. Does not load
/// anything and does not check that the directory exists; `inspectionRuntimeAt`
/// followed by a call is how absence is discovered and reported.
InspectionRuntime inspectionRuntimeAt(const QString& directory);

/// The runtime named by `MIKAI_ORT_RUNTIME_DIR`, or an empty record. The
/// environment variable is an explicit directory, never a PATH entry.
InspectionRuntime defaultInspectionRuntime();

/// True when the module exists as a regular file directly inside the named
/// directory. Does not load it.
bool inspectionRuntimeIsPresent(const InspectionRuntime& runtime, QString* reason = nullptr);

/// What one graph's declared boundary is, and - where a run was asked for - what
/// its CPU execution actually produced.
///
/// Everything in `declared` is what the graph states. A dimension the graph left
/// dynamic stays absent rather than becoming zero or a placeholder, exactly as
/// `TensorProfile` requires.
struct InspectionResult {
  bool ok = false;
  /// Stable key and sentence; set exactly when `ok` is false, and also used by
  /// `referenceKey`/`referenceReason` for a declared-read that succeeded while
  /// the CPU run did not.
  QString reasonKey;
  QString reason;

  QString inputName;
  QString outputName;
  ModelProfile declared;

  /// True once a CPU reference run returned and its output tensor's type and
  /// shape were read.
  bool referenceRan = false;
  /// The geometry the reference ran at. Invalid when no run happened.
  QSize runGeometry;
  /// The geometry the run actually produced. Absent when it produced no usable
  /// boundary: a wrong rank, a batch or channel count this bridge cannot bind, a
  /// non-positive H/W, or a size beyond the probe's allocation bound.
  std::optional<QSize> resolvedGeometry;
  /// The uniform positive integer scale the run's output implies, learned from
  /// the shapes - never read from a file name.
  std::optional<int> resolvedScale;

  /// Evidence, not a declaration: the concrete 1x3xHxW fp16 boundary the CPU
  /// reference actually bound (input) and returned (output), recorded only after
  /// the run's own output was checked against this bridge's dtype, rank, N, C and
  /// size rules. `declared` is never rewritten from it - a symbolic declared
  /// batch stays symbolic and the concrete batch measured here is separate
  /// evidence, never a replacement for what the graph stated. Whether the
  /// evidence proves a particular declaration is what `spatialReferenceRefusal`
  /// decides. Absent until a run's boundary has been verified.
  std::optional<ModelProfile> verifiedReferenceProfile;

  /// Why the reference run did not produce a geometry; empty when it did or was
  /// never attempted, and set with a name when a declared boundary was not even a
  /// runnable candidate.
  QString referenceKey;
  QString referenceReason;

  bool hasDeclared() const { return ok; }
};

/// Reads the declared input/output boundary of `bytes`. Exactly one input and one
/// output are required; a graph with more or fewer is refused naming the actual
/// counts. A truncated or otherwise unreadable graph is refused here, before any
/// caller can publish it. Never executes the graph.
InspectionResult inspectDeclaredProfile(const InspectionRuntime& runtime, const QByteArray& bytes);

/// Declared profile plus a CPU reference run (fp16 NCHW).
///
/// A run happens only for a graph whose declared tensors are both FP16 - the
/// only dtype this bridge can bind - and only when something is left for a run to
/// learn: a dynamic H/W, or a symbolic declared batch even where H and W are
/// fixed. A fully concrete declaration is answered without executing anything. A
/// graph whose declaration is not even a bindable candidate is named
/// (`inspect.reference.ineligible`), never executed.
///
/// A dynamic graph is run at `sourceGeometry`; a graph with a fixed input is run
/// at the geometry it declares, because that is the only size it can be fed. What
/// comes back is checked against this bridge's rules - the dtype ORT reports,
/// rank 4, N=1, C=3, and positive bounded H/W - before any geometry, scale or
/// proof is recorded, and the concrete boundary the run produced is kept in
/// `verifiedReferenceProfile` beside the untouched `declared`. A run that
/// returned no such boundary fills `referenceKey`/`referenceReason` instead: the
/// declaration and the run's outcome are separate facts.
InspectionResult inspectGraph(const InspectionRuntime& runtime, const QByteArray& bytes,
                              const QSize& sourceGeometry);

/// The geometry one model resolves to at one stage's source size.
struct StageResolution {
  /// The geometry the model is fed: the declared input for a fixed graph, the
  /// requested stage geometry for a dynamic one.
  QSize input;
  /// The geometry it produces.
  QSize output;
  /// The uniform positive integer factor between the two, identity included.
  int scale = 0;
  /// True when `output` came from a CPU reference run rather than from the
  /// graph's own fixed declaration.
  bool fromCpuReference = false;
  /// Evidence, not a declaration: the concrete 1x3xHxW fp16 boundary the CPU
  /// reference actually bound and returned, and which `spatialReferenceRefusal`
  /// accepted as proving `declared`. Present exactly when `fromCpuReference` is
  /// true; absent for a fixed graph answered from its own declaration.
  std::optional<ModelProfile> verifiedReferenceProfile;
};

/// Resolve a graph's stage geometry.
///
/// A strictly eligible, fully fixed graph answers from its own declared geometry
/// and is not executed. Every other graph - dynamic H/W, or a declared symbolic
/// output batch - is executed on the CPU and answered from what it *actually
/// produced*, with the scale learned from those shapes. `onnxBytes` must be the
/// graph's bytes; it is used only when a run is needed.
///
/// Returns nothing, with a reason, when the runtime cannot be loaded, the graph
/// cannot be read, the declared boundary is not a candidate, or the run did not
/// produce a uniform positive integer boundary whose actual tensors prove the
/// declaration (`spatialReferenceRefusal`). Nothing is defaulted and no geometry
/// is fabricated, and the proof the answer carries is the run's own boundary, not
/// the declaration restated.
std::optional<StageResolution> resolveStageGeometry(const ModelProfile& declared,
                                                    const QSize& sourceGeometry,
                                                    const QByteArray& onnxBytes,
                                                    const InspectionRuntime& runtime,
                                                    QString* reasonKey = nullptr,
                                                    QString* reason = nullptr);

}  // namespace native
}  // namespace mplayer
