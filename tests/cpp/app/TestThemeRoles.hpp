#pragma once

#include <QtTest/QTest>

#include <memory>

#include <QtCore/QString>
#include <QtCore/QTemporaryDir>
#include <QtGui/QColor>
#include <QtGui/QPalette>

#include "theme/Appearance.hpp"

// Consumer-visible contract of GOAL-A: one accent token, four action roles and
// a gradient that follows the accent unless the viewer overrode it. Every
// rendering assertion measures pixels produced by the real stylesheet on real
// widgets; every ratio is recomputed from the tokens with an independent WCAG
// implementation rather than trusted from the production helper.
class TestThemeRoles : public QObject {
    Q_OBJECT

 private slots:
  void initTestCase();
  void init();
  void cleanup();

  void actionRoleStatesOnePropertyAndNoSize();
  void searchPrimaryFillIsTheRawAccent();
  void secondaryBoundaryPassesThreeToOneInBothThemes();
  void roleStatesRenderTheirDocumentedContract();
  void primaryInkStaysReadableOnEveryAccent();
  void compareSideBStaysAmberOrRotatesToTheComplement();
  void gradientModeMigratesAndPreservesTheCustomChoice();
  void swatchKeepsItsFixedSizeInsideAFlowLayout();

 private:
  std::unique_ptr<QTemporaryDir> settingsDirectory_;
  mikai::app::theme::Appearance savedAppearance_;
  QPalette savedPalette_;
  QString savedStylesheet_;
  QColor savedAccent_;
};
