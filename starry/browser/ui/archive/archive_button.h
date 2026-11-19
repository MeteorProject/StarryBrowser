// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_BUTTON_H_
#define STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_BUTTON_H_

#include "base/scoped_observation.h"
#include "starry/browser/tab_organizer/tab_archive.h"
#include "starry/browser/ui/design/controls.h"
#include "ui/base/metadata/metadata_header_macros.h"

class BrowserWindowInterface;

namespace starry {

// 侧栏顶栏右端的「已归档」图标。在标签页上有归档时显示。
class ArchiveButton : public IconButton,
                      public tab_organizer::TabArchive::Observer {
  METADATA_HEADER(ArchiveButton, IconButton)

 public:
  explicit ArchiveButton(BrowserWindowInterface* browser);
  ArchiveButton(const ArchiveButton&) = delete;
  ArchiveButton& operator=(const ArchiveButton&) = delete;
  ~ArchiveButton() override;

  void SetTabsPageVisible(bool visible);

 private:
  // tab_organizer::TabArchive::Observer:
  void OnArchiveChanged() override;

  bool tabs_page_visible_ = true;
  base::ScopedObservation<tab_organizer::TabArchive,
                          tab_organizer::TabArchive::Observer>
      observation_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_BUTTON_H_
