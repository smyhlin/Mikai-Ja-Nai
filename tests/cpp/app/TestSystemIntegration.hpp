#pragma once

#include <QtTest/QTest>

class TestSystemIntegration : public QObject {
  Q_OBJECT

private slots:
  void startupRegistrationRoundTripsInIsolatedRegistry();
  void startupRefusesAndPreservesForeignEntry();
  void contextMenuRegistrationRoundTripsInIsolatedRegistry();
  void contextMenuRefusesForeignVerbWithoutChangingIt();
  void contextMenuPartialFailureRestoresPriorValues();
  void defaultPlayerRefusesForeignOwnerWithoutOpeningSettings();
  void defaultPlayerPartialFailureRollsBackOwnedValues();
  void defaultPlayerSettingsLaunchFailureRetainsRegistration();
};
