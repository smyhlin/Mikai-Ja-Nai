#include "TestFavouriteTeams.hpp"

#include "app/state/FavouriteTeams.hpp"

#include <QSettings>
#include <QtTest/QTest>

using namespace mikai::app;

void TestFavouriteTeams::saveLoadCheckAndClearFavouriteTeam() {
  const QString testKey = QStringLiteral("test/favouriteTeamsUnit");
  QSettings().remove(testKey);

  FavouriteTeams store(testKey);
  QVERIFY(!store.favourite(QStringLiteral("7428")).has_value());

  store.setFavourite(QStringLiteral("7428"), QStringLiteral("MoonAnime"),
                     QStringLiteral("fanvoxua"), QStringLiteral("rel-1"));
  QVERIFY(store.isFavourite(QStringLiteral("7428"), QStringLiteral("moonanime"),
                            QStringLiteral("fanvoxua")));
  QVERIFY(store.isFavourite(QStringLiteral("7428"), QStringLiteral("moon"),
                            QStringLiteral("FanVoxUA")));

  const auto loaded = store.favourite(QStringLiteral("7428"));
  QVERIFY(loaded.has_value());
  QCOMPARE(loaded->provider, QStringLiteral("moonanime"));
  QCOMPARE(loaded->teamSlug, QStringLiteral("fanvoxua"));
  QCOMPARE(loaded->releaseId, QStringLiteral("rel-1"));

  store.clearFavourite(QStringLiteral("7428"));
  QVERIFY(!store.favourite(QStringLiteral("7428")).has_value());
  QSettings().remove(testKey);
}
