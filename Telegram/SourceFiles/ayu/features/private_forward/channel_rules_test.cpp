// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/private_forward/channel_rules.h"

#include <iostream>

namespace {

int gFailures = 0;

void Expect(bool condition, const char *name) {
	if (!condition) {
		std::cerr << "FAIL " << name << '\n';
		++gFailures;
	}
}

Ayu::PrivateForward::ChannelMessage Post(std::int64_t peer, std::int32_t msg) {
	auto message = Ayu::PrivateForward::ChannelMessage();
	message.peerId = peer;
	message.msgId = msg;
	message.dateUnix = 5'000;
	message.broadcast = true;
	message.sourceAccessible = true;
	return message;
}

} // namespace

int main() {
	using namespace Ayu::PrivateForward;
	const auto canSend = [](std::int64_t peer) {
		return peer == 10 || peer == 20 || peer == 21 || peer == 30 || peer == 40;
	};

	{
		auto rules = ChannelRules(7);
		Expect(!rules.globalEnabled(), "global default off");
		Expect(rules.rules().empty(), "no rules");
		auto draft = ChannelRule();
		draft.sourcePeerId = 10;
		draft.targetPeerId = 20;
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::Group, true)
				== RuleError::None,
			"add");
		Expect(!rules.rules().front().enabled, "rule default off");
		const auto routed = rules.route(Post(10, 1), canSend);
		Expect(routed.size() == 1, "global skip one");
		Expect(routed.front().skip == ChannelSkip::GlobalOff, "global off");
		Expect(!routed.front().forward, "global does not forward");
	}

	{
		auto rules = ChannelRules(7);
		auto draft = ChannelRule();
		draft.sourcePeerId = 10;
		draft.targetPeerId = 10;
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::Broadcast, true)
				== RuleError::SamePeer,
			"same peer");
		draft.targetPeerId = 20;
		Expect(
			rules.addRule(draft, PeerKind::Group, PeerKind::Group, true)
				== RuleError::SourceNotBroadcast,
			"group source");
		Expect(
			rules.addRule(draft, PeerKind::User, PeerKind::Group, true)
				== RuleError::SourceNotBroadcast,
			"user source");
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::User, false)
				== RuleError::TargetNotSendable,
			"user target");
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::Group, false)
				== RuleError::TargetNotSendable,
			"no send");
		draft.topicId = 4;
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::Group, true)
				== RuleError::TopicUnexpected,
			"topic on group");
		draft.topicId = 0;
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::Forum, true)
				== RuleError::TopicRequired,
			"forum needs topic");
		draft.topicId = 8;
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::Forum, true)
				== RuleError::None,
			"forum with topic");
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::Forum, true)
				== RuleError::Duplicate,
			"duplicate");
	}

	{
		auto rules = ChannelRules(7);
		auto draft = ChannelRule();
		draft.sourcePeerId = 10;
		draft.targetPeerId = 20;
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::Group, true)
				== RuleError::None,
			"rule");
		const auto id = rules.rules().front().id;
		rules.setGlobalEnabled(true, 100);
		auto early = rules.route(Post(10, 1), canSend);
		Expect(early.front().skip == ChannelSkip::RuleOff, "rule off");
		Expect(
			rules.setRuleEnabled(id, true, 0) == RuleError::Missing,
			"refuse zero time");
		Expect(rules.setRuleEnabled(id, true, 200) == RuleError::None, "arm");
		auto message = Post(10, 2);
		message.dateUnix = 150;
		Expect(
			rules.route(message, canSend).front().skip
				== ChannelSkip::BeforeEnable,
			"before enable");
		message.dateUnix = 200;
		auto ready = rules.route(message, canSend);
		Expect(ready.front().forward, "at enable time");
		Expect(ready.front().targetPeerId == 20, "target");
		message.dateUnix = 199;
		Expect(
			rules.route(message, canSend).front().skip
				== ChannelSkip::BeforeEnable,
			"one second early");
		message = Post(10, 3);
		message.broadcast = false;
		Expect(
			rules.route(message, canSend).front().skip
				== ChannelSkip::NotBroadcast,
			"not channel");
		message = Post(10, 4);
		message.protectedContent = true;
		Expect(
			rules.route(message, canSend).front().skip
				== ChannelSkip::Protected,
			"protected");
		message = Post(10, 5);
		message.service = true;
		Expect(
			rules.route(message, canSend).front().skip == ChannelSkip::Service,
			"service");
		message = Post(11, 6);
		message.sourceAccessible = false;
		Expect(
			rules.route(message, canSend).front().skip == ChannelSkip::Deleted,
			"deleted source");
	}

	{
		auto rules = ChannelRules(7);
		auto first = ChannelRule();
		first.sourcePeerId = 10;
		first.targetPeerId = 20;
		auto second = ChannelRule();
		second.sourcePeerId = 10;
		second.targetPeerId = 21;
		Expect(
			rules.addRule(first, PeerKind::Broadcast, PeerKind::Group, true)
				== RuleError::None,
			"add first");
		Expect(
			rules.addRule(
				second,
				PeerKind::Broadcast,
				PeerKind::Broadcast,
				true) == RuleError::None,
			"add second");
		rules.setGlobalEnabled(true, 10);
		Expect(
			rules.setRuleEnabled(rules.rules()[0].id, true, 100)
				== RuleError::None,
			"arm first");
		Expect(
			rules.setRuleEnabled(rules.rules()[1].id, true, 100)
				== RuleError::None,
			"arm second");
		const auto routes = rules.route(Post(10, 9), canSend);
		Expect(routes.size() == 2, "two targets");
		Expect(routes[0].forward && routes[1].forward, "both forward");
		Expect(routes[0].targetPeerId != routes[1].targetPeerId, "distinct");
		Expect(rules.claim(routes[0].ruleId, 10, 9), "claim");
		Expect(!rules.claim(routes[0].ruleId, 10, 9), "dedup");
		Expect(
			rules.route(Post(10, 9), canSend)[0].skip == ChannelSkip::Duplicate,
			"duplicate route");
		Expect(rules.route(Post(10, 9), canSend)[1].forward, "other rule");
	}

	{
		auto rules = ChannelRules(7);
		auto ab = ChannelRule();
		ab.sourcePeerId = 10;
		ab.targetPeerId = 20;
		auto ba = ChannelRule();
		ba.sourcePeerId = 20;
		ba.targetPeerId = 10;
		Expect(
			rules.addRule(ab, PeerKind::Broadcast, PeerKind::Broadcast, true)
				== RuleError::None,
			"add ab");
		Expect(
			rules.addRule(ba, PeerKind::Broadcast, PeerKind::Broadcast, true)
				== RuleError::None,
			"add ba");
		rules.setGlobalEnabled(true, 10);
		for (const auto &rule : rules.rules()) {
			Expect(
				rules.setRuleEnabled(rule.id, true, 50) == RuleError::None,
				"arm loop rule");
		}
		Expect(rules.claim(rules.rules()[0].id, 10, 3), "took origin");
		rules.rememberProduced(20, 80);
		auto copy = Post(20, 80);
		copy.forwarded = true;
		copy.forwardFromPeer = 10;
		copy.forwardFromMsg = 3;
		Expect(
			rules.route(copy, canSend).front().skip == ChannelSkip::Loop,
			"produced copy");
		auto another = Post(20, 81);
		another.forwarded = true;
		another.forwardFromPeer = 10;
		another.forwardFromMsg = 3;
		Expect(
			rules.route(another, canSend).front().skip == ChannelSkip::Loop,
			"origin loop");
		auto fresh = Post(20, 82);
		fresh.dateUnix = 80;
		Expect(rules.route(fresh, canSend).front().forward, "real channel post");
	}

	{
		auto rules = ChannelRules(1);
		auto draft = ChannelRule();
		draft.sourcePeerId = 10;
		draft.targetPeerId = 20;
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::Group, true)
				== RuleError::None,
			"persist add");
		rules.setGlobalEnabled(true, 5);
		Expect(
			rules.setRuleEnabled(rules.rules().front().id, true, 9)
				== RuleError::None,
			"persist arm");
		Expect(rules.claim(rules.rules().front().id, 10, 1), "persist claim");
		rules.rememberProduced(20, 2);
		rules.noteSuccess(20261117, 9, 10, 1, 20, 1);
		const auto dumped = rules.dump();
		auto other = ChannelRules(2);
		Expect(!other.load(dumped, 2), "reject other account");
		Expect(!other.globalEnabled(), "other stays off");
		Expect(other.rules().empty(), "other has no rules");
		auto same = ChannelRules(1);
		Expect(same.load(dumped, 1), "same account");
		Expect(same.globalEnabled(), "loaded global");
		Expect(same.rules().size() == 1, "loaded rule");
		Expect(same.rules().front().enabledAt == 9, "loaded cutoff");
		Expect(same.originTaken(10, 1), "loaded origin");
		Expect(same.isProduced(20, 2), "loaded produced");
		Expect(same.forwardedOn(20261117) == 1, "loaded count");
		Expect(!same.load("{", 1), "bad json");
		Expect(same.globalEnabled(), "bad json keeps state");
	}

	{
		auto rules = ChannelRules(7);
		auto draft = ChannelRule();
		draft.sourcePeerId = 10;
		draft.targetPeerId = 30;
		Expect(
			rules.addRule(draft, PeerKind::Broadcast, PeerKind::Group, true)
				== RuleError::None,
			"rearm add");
		rules.setGlobalEnabled(false, 1);
		rules.setGlobalEnabled(true, 500);
		Expect(
			rules.setRuleEnabled(rules.rules().front().id, true, 100)
				== RuleError::None,
			"rearm rule");
		rules.setGlobalEnabled(true, 800);
		auto message = Post(10, 4);
		message.dateUnix = 700;
		Expect(
			rules.route(message, canSend).front().skip
				== ChannelSkip::BeforeEnable,
			"global rearm cutoff");
		message.dateUnix = 800;
		Expect(rules.route(message, canSend).front().forward, "after rearm");
		const auto blocked = [](std::int64_t) { return false; };
		Expect(
			rules.route(message, blocked).front().skip
				== ChannelSkip::NoPermission,
			"permission");
	}

	if (gFailures) {
		std::cerr << gFailures << " failed\n";
		return 1;
	}
	std::cout << "ok\n";
	return 0;
}
