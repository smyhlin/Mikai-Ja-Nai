#pragma once

#include <QObject>

class TestFavouriteTeams : public QObject {
  Q_OBJECT

 private slots:
  void saveLoadCheckAndClearFavouriteTeam();
};
