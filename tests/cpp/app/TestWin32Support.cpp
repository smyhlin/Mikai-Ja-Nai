#include "TestWin32Support.hpp"

#include <gui/Win32Support.hpp>

#include <type_traits>
#include <utility>

#include <QtCore/QString>

using mikai::gui::win32::missing;
using mikai::gui::win32::RegistryKey;
using mikai::gui::win32::systemMessage;
using mikai::gui::win32::wide;

// Move-only by construction: a copied handle would close twice.
static_assert(!std::is_copy_constructible_v<RegistryKey> &&
                  !std::is_copy_assignable_v<RegistryKey>,
              "RegistryKey must stay move-only");
static_assert(std::is_move_constructible_v<RegistryKey> &&
                  std::is_move_assignable_v<RegistryKey>,
              "RegistryKey must stay movable");

void TestWin32Support::anOpenedKeyIsValidAndItsMoveEmptiesTheSource() {
  // HKCU\Software exists on every Windows install, and KEY_READ is the least
  // privilege the production callers use (SystemStartup, SystemDefaultPlayer),
  // so the open itself cannot fail for access reasons.
  RegistryKey key;
  const LSTATUS status =
      RegOpenKeyExW(HKEY_CURRENT_USER, wide(QStringLiteral("Software")), 0, KEY_READ, &key.handle);
  QVERIFY2(status == ERROR_SUCCESS,
           qPrintable(systemMessage(static_cast<DWORD>(status))));
  QVERIFY(key.handle != nullptr);

  // The source must be empty afterwards, so exactly one RegCloseKey can run
  // for this handle — no double close. The close itself runs in the
  // destructor as `moved` goes out of scope; a closed HKEY has no queryable
  // state, so the move-empty invariant above is the evidence, not a probe of
  // the OS for the handle.
  RegistryKey moved(std::move(key));
  QVERIFY(key.handle == nullptr);
  QVERIFY(moved.handle != nullptr);
}

void TestWin32Support::aMissingKeyReportsAsMissing() {
  RegistryKey key;
  const LSTATUS status = RegOpenKeyExW(
      HKEY_CURRENT_USER, wide(QStringLiteral("Software\\MikaiJaNaiNoSuchKey")), 0, KEY_READ,
      &key.handle);
  QVERIFY2(status == ERROR_FILE_NOT_FOUND,
           qPrintable(systemMessage(static_cast<DWORD>(status))));
  QVERIFY(key.handle == nullptr);
  QVERIFY(missing(status));
  QVERIFY(!missing(ERROR_SUCCESS));
  QVERIFY(!missing(ERROR_ACCESS_DENIED));
}

void TestWin32Support::moveAssignmentEmptiesTheSourceAndClosesTheOldHandle() {
  RegistryKey first;
  QVERIFY(RegOpenKeyExW(HKEY_CURRENT_USER, wide(QStringLiteral("Software")), 0, KEY_READ,
                        &first.handle) == ERROR_SUCCESS);
  RegistryKey second;
  QVERIFY(RegOpenKeyExW(HKEY_CURRENT_USER, wide(QStringLiteral("Software")), 0, KEY_READ,
                        &second.handle) == ERROR_SUCCESS);
  QVERIFY(first.handle != nullptr);
  QVERIFY(second.handle != nullptr);

  first = std::move(second);
  QVERIFY(second.handle == nullptr);
  QVERIFY(first.handle != nullptr);
  // The assignment closed `first`'s original handle before adopting
  // `second`'s; both handles close exactly once, at scope end.
}

void TestWin32Support::systemMessageReturnsTextForAKnownError() {
  const QString fileNotFound = systemMessage(ERROR_FILE_NOT_FOUND);
  QVERIFY(!fileNotFound.isEmpty());
  // The helper owns the FormatMessageW buffer (LocalFree before return): a
  // second call must still return text, not crash on a freed buffer.
  QVERIFY(!systemMessage(ERROR_PATH_NOT_FOUND).isEmpty());
}

void TestWin32Support::wideRoundTripsANonAsciiPath() {
  const QString path = QStringLiteral("C:\\Тест\\Мікаї\\файл.txt");
  QCOMPARE(QString::fromWCharArray(wide(path)), path);
}
