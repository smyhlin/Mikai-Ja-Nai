#pragma once

#include <QMetaType>

namespace mplayer {
namespace shell {

/// Where a processing-failure remedy wants the app's existing Settings surface
/// to go. A typed intent rather than a bool, because the two answers are not
/// the same: Providers only brings the existing choice into view and changes
/// nothing, while Prepare additionally runs the existing preparation command -
/// and only after an explicit click, never because settings were opened.
///
/// This is deliberately not a request to fetch anything: no URL is re-extracted,
/// no quality is re-probed and no media state is touched on this path. Original
/// needs no intent at all, because the dialog answers it itself through the
/// existing native turn-off request.
enum class ProcessingSettingsIntent { Prepare, Providers };

}  // namespace shell
}  // namespace mplayer

Q_DECLARE_METATYPE(mplayer::shell::ProcessingSettingsIntent)