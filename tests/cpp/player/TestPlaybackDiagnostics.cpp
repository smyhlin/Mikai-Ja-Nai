#include "TestPlaybackDiagnostics.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "MpvHandle.hpp"
#include "PlaybackDiagnostics.hpp"

using mplayer::AsyncReply;
using mplayer::NodeValue;

namespace {

AsyncReply int64Reply(int64_t value) {
  AsyncReply reply;
  reply.node.format = MPV_FORMAT_INT64;
  reply.node.integer = value;
  return reply;
}

NodeValue stringValue(const std::string& text) {
  NodeValue value;
  value.format = MPV_FORMAT_STRING;
  value.text = text;
  return value;
}

NodeValue vfRow(const std::string& name, const std::string& label = std::string()) {
  NodeValue row;
  row.format = MPV_FORMAT_NODE_MAP;
  row.map.emplace_back("name", stringValue(name));
  if (!label.empty()) row.map.emplace_back("label", stringValue(label));
  return row;
}

AsyncReply vfChainReply(std::vector<NodeValue> rows) {
  AsyncReply reply;
  reply.node.format = MPV_FORMAT_NODE_ARRAY;
  reply.node.list = std::move(rows);
  return reply;
}

bool vfReadable(const AsyncReply& reply) {
  bool present = false;
  int count = 0;
  return mplayer::summarizeVfChain(reply, &present, &count);
}

}  // namespace

void TestPlaybackDiagnostics::anIntegerBeyondTwoToTheFiftyThirdIsRetainedExactly() {
  // Double transport rounds 2^53 + 1; counters must retain int64 precision.
  const int64_t beyondExact = (int64_t(1) << 53) + 1;
  const auto value = mplayer::exactInteger(int64Reply(beyondExact));
  QVERIFY(value.has_value());
  QCOMPARE(*value, beyondExact);
}

void TestPlaybackDiagnostics::aCounterThatIsUnavailableOrNotAnIntegerIsUnknownNeverZero() {
  AsyncReply unavailable;
  unavailable.error = MPV_ERROR_PROPERTY_UNAVAILABLE;
  unavailable.node.format = MPV_FORMAT_INT64;
  unavailable.node.integer = 4;
  const auto refused = mplayer::exactInteger(unavailable);
  QVERIFY(!refused.has_value());

  AsyncReply wrongType;
  wrongType.node.format = MPV_FORMAT_DOUBLE;
  wrongType.node.number = 0.0;
  const auto mistyped = mplayer::exactInteger(wrongType);
  QVERIFY(!mistyped.has_value());

  AsyncReply none;
  const auto absent = mplayer::exactInteger(none);
  QVERIFY(!absent.has_value());
}

void TestPlaybackDiagnostics::anUnreadableVfReplyIsNeverProvenAbsent() {
  AsyncReply scalar;
  scalar.node.format = MPV_FORMAT_INT64;
  scalar.node.integer = 0;
  QVERIFY(!vfReadable(scalar));

  AsyncReply refused = vfChainReply({});
  refused.error = MPV_ERROR_PROPERTY_UNAVAILABLE;
  QVERIFY(!vfReadable(refused));

  NodeValue notMap;
  notMap.format = MPV_FORMAT_STRING;
  notMap.text = "lavfi";
  QVERIFY(!vfReadable(vfChainReply({notMap})));

  NodeValue noName;
  noName.format = MPV_FORMAT_NODE_MAP;
  noName.map.emplace_back("label", stringValue("ordinary"));
  QVERIFY(!vfReadable(vfChainReply({noName})));

  NodeValue wrongName;
  wrongName.format = MPV_FORMAT_NODE_MAP;
  NodeValue nameNumber;
  nameNumber.format = MPV_FORMAT_INT64;
  nameNumber.integer = 4;
  wrongName.map.emplace_back("name", nameNumber);
  QVERIFY(!vfReadable(vfChainReply({wrongName})));

  NodeValue wrongLabel;
  wrongLabel.format = MPV_FORMAT_NODE_MAP;
  wrongLabel.map.emplace_back("name", stringValue("lavfi"));
  NodeValue labelNumber;
  labelNumber.format = MPV_FORMAT_INT64;
  labelNumber.integer = 1;
  wrongLabel.map.emplace_back("label", labelNumber);
  QVERIFY(!vfReadable(vfChainReply({wrongLabel})));

  NodeValue duplicateName;
  duplicateName.format = MPV_FORMAT_NODE_MAP;
  duplicateName.map.emplace_back("name", stringValue("lavfi"));
  duplicateName.map.emplace_back("name", stringValue("animejanai"));
  QVERIFY(!vfReadable(vfChainReply({duplicateName})));

  NodeValue duplicateLabel;
  duplicateLabel.format = MPV_FORMAT_NODE_MAP;
  duplicateLabel.map.emplace_back("name", stringValue("lavfi"));
  duplicateLabel.map.emplace_back("label", stringValue("ordinary"));
  duplicateLabel.map.emplace_back("label", stringValue("mikai_native"));
  QVERIFY(!vfReadable(vfChainReply({duplicateLabel})));
}

void TestPlaybackDiagnostics::anEmptyChainAndAnOrdinaryFilterProveNativeAbsent() {
  bool present = true;
  int count = 99;
  QVERIFY(mplayer::summarizeVfChain(vfChainReply({}), &present, &count));
  QVERIFY(!present);
  QCOMPARE(count, 0);

  present = true;
  count = 99;
  QVERIFY(mplayer::summarizeVfChain(vfChainReply({vfRow("lavfi")}), &present, &count));
  QVERIFY(!present);
  QCOMPARE(count, 1);
}

void TestPlaybackDiagnostics::theNativeFilterNameOrLabelProvesPresence() {
  bool present = false;
  int count = 0;
  QVERIFY(mplayer::summarizeVfChain(vfChainReply({vfRow("animejanai")}), &present, &count));
  QVERIFY(present);
  QCOMPARE(count, 1);

  present = false;
  count = 0;
  QVERIFY(mplayer::summarizeVfChain(vfChainReply({vfRow("unknown", "mikai_native")}),
                                    &present, &count));
  QVERIFY(present);
  QCOMPARE(count, 1);
}
