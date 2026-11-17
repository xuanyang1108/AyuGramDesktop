// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/private_forward/forward_policy.h"

#include <iostream>
#include <string>

namespace {

int gFailures = 0;

void Expect(bool condition, const char *name) {
	if (!condition) {
		std::cerr << "FAIL " << name << '\n';
		++gFailures;
	}
}

Ayu::PrivateForward::Settings Enabled() {
	auto settings = Ayu::PrivateForward::Settings();
	settings.enabled = true;
	settings.targetPeerId = 9001;
	return settings;
}

Ayu::PrivateForward::Snapshot PrivateText(std::int64_t peer, std::int32_t msg) {
	auto message = Ayu::PrivateForward::Snapshot();
	message.peerId = peer;
	message.msgId = msg;
	message.privateUser = true;
	message.kind = Ayu::PrivateForward::ContentKind::Text;
	message.dateUnix = 1'700'000'000;
	return message;
}

} // namespace

int main() {
	using namespace Ayu::PrivateForward;

	{
		auto engine = Engine(42);
		Expect(!engine.settings().enabled, "default off");
		Expect(!engine.settings().relayReplies, "relay default off");
		Expect(!engine.settings().allowOtherAdmins, "admins default off");
		Expect(!engine.running(), "not running");
		const auto decision = engine.decideForward(PrivateText(7, 1));
		Expect(decision.action == Action::Ignore, "disabled ignores");
		Expect(decision.skip == Skip::Disabled, "disabled reason");
	}

	{
		auto engine = Engine(1);
		engine.setSettings(Enabled());
		auto message = PrivateText(7, 1);
		message.savedMessages = true;
		Expect(
			engine.decideForward(message).skip == Skip::SavedMessages,
			"saved");
		message = PrivateText(7, 1);
		message.serviceChat = true;
		Expect(
			engine.decideForward(message).skip == Skip::ServiceChat,
			"service");
		message = PrivateText(7, 1);
		message.authSecret = true;
		Expect(
			engine.decideForward(message).skip == Skip::AuthSecret,
			"auth");
		message = PrivateText(7, 1);
		message.privateUser = false;
		Expect(
			engine.decideForward(message).skip == Skip::NotPrivate,
			"group");
		message = PrivateText(7, 1);
		message.inTargetForum = true;
		Expect(
			engine.decideForward(message).skip == Skip::NotPrivate,
			"forum source");
		message = PrivateText(7, 1);
		message.protectedContent = true;
		Expect(
			engine.decideForward(message).skip == Skip::ProtectedContent,
			"protected");
		message = PrivateText(7, 1);
		message.bot = true;
		Expect(engine.decideForward(message).skip == Skip::Bot, "bot");
		message = PrivateText(7, 1);
		message.muted = true;
		auto settings = Enabled();
		settings.ignoreMuted = true;
		engine.setSettings(settings);
		Expect(engine.decideForward(message).skip == Skip::Muted, "muted");
		settings.excluded = { 7 };
		engine.setSettings(settings);
		message.muted = false;
		Expect(
			engine.decideForward(message).skip == Skip::Excluded,
			"excluded");
		settings = Enabled();
		settings.forwardOwn = false;
		engine.setSettings(settings);
		message = PrivateText(7, 1);
		message.outgoing = true;
		Expect(
			engine.decideForward(message).skip == Skip::OwnMessage,
			"own");
		settings.allowText = false;
		settings.forwardOwn = true;
		engine.setSettings(settings);
		message.outgoing = false;
		Expect(
			engine.decideForward(message).skip == Skip::Filtered,
			"filter");
		settings.allowText = true;
		settings.perPeerTopic = false;
		engine.setSettings(settings);
		Expect(
			engine.decideForward(message).skip == Skip::TopicsOff,
			"topics off");
	}

	{
		auto engine = Engine(1);
		engine.setSettings(Enabled());
		const auto message = PrivateText(7, 5);
		Expect(
			engine.decideForward(message).action == Action::Forward,
			"forward");
		Expect(engine.claimForward(7, 5), "claim");
		Expect(!engine.claimForward(7, 5), "dedup claim");
		Expect(
			engine.decideForward(message).skip == Skip::Duplicate,
			"dedup decide");
		engine.releaseForward(7, 5);
		Expect(engine.claimForward(7, 5), "reclaim");
	}

	{
		auto engine = Engine(1);
		auto settings = Enabled();
		settings.relayReplies = true;
		settings.relayEnabledAt = 100;
		engine.setSettings(settings);
		engine.bindTopic(7, 50);
		auto message = PrivateText(9001, 80);
		message.privateUser = false;
		message.inTargetForum = true;
		message.topicRootId = 50;
		message.authorIsSelf = true;
		message.loopMark = true;
		Expect(engine.decideRelay(message).skip == Skip::Echo, "mark echo");
		message.loopMark = false;
		engine.rememberProducedForum(80);
		Expect(engine.decideRelay(message).skip == Skip::Echo, "id echo");
		message.msgId = 81;
		Expect(
			engine.decideRelay(message).action == Action::Relay,
			"self relay");
		message.authorIsSelf = false;
		Expect(
			engine.decideRelay(message).skip == Skip::GroupOnly,
			"other member");
		message.forwarded = true;
		message.authorIsSelf = true;
		Expect(
			engine.decideRelay(message).skip == Skip::Forwarded,
			"forwarded topic msg");
		message.forwarded = false;
		message.authorIsSelf = false;
		message.anonymousAdmin = true;
		settings.allowOtherAdmins = true;
		engine.setSettings(settings);
		Expect(
			engine.decideRelay(message).skip == Skip::Anonymous,
			"anonymous");
		message.anonymousAdmin = false;
		message.authorIsBot = true;
		Expect(
			engine.decideRelay(message).skip == Skip::NotHuman,
			"admin bot");
		message.authorIsBot = false;
		Expect(
			engine.decideRelay(message).skip == Skip::NeedVerify,
			"need verify");
		message.verifiedAdmin = true;
		Expect(
			engine.decideRelay(message).action == Action::Relay,
			"verified admin");
		settings.relayReplies = false;
		engine.setSettings(settings);
		Expect(!engine.settings().allowOtherAdmins, "admins cleared");
		Expect(
			engine.decideRelay(message).skip == Skip::RelayOff,
			"relay off");
	}

	{
		auto first = Engine(11);
		auto settings = Enabled();
		settings.excluded = { 3, 4 };
		first.setSettings(settings);
		first.bindTopic(8, 15);
		Expect(first.claimForward(8, 2), "iso claim");
		first.rememberProducedForum(15);
		first.noteForwardSuccess(20261117, 10, 8, 2, 15, 2);
		const auto dumped = first.dump();
		auto other = Engine(22);
		Expect(!other.load(dumped, 22), "reject other account");
		Expect(!other.settings().enabled, "other stays default");
		Expect(!other.topicFor(8), "other has no topic");
		auto same = Engine(11);
		Expect(same.load(dumped, 11), "load same account");
		Expect(same.settings().enabled, "loaded enabled");
		Expect(same.settings().excluded.size() == 2, "loaded exclude");
		Expect(same.topicFor(8) == 15, "loaded topic");
		Expect(same.wasForwarded(8, 2), "loaded dedup");
		Expect(same.isProducedForum(15), "loaded produced");
		Expect(same.forwardedOn(20261117) == 2, "loaded count");
		Expect(same.log().size() == 1, "loaded log");
		Expect(same.log().front().code == "forwarded", "log code only");
	}

	{
		auto engine = Engine(1);
		engine.noteFlood(5, 1000);
		Expect(engine.floodPaused(1000), "flood start");
		Expect(engine.floodPaused(1004), "flood mid");
		Expect(engine.floodWaitLeft(1002) == 3, "flood left");
		Expect(!engine.floodPaused(1005), "flood done");
		engine.noteError(1004, "FLOOD_WAIT_5", 7, 1);
		Expect(engine.lastError() == "FLOOD_WAIT_5", "error kept");
		engine.noteForwardSuccess(20261117, 1006, 7, 1, 3, 1);
		Expect(engine.lastError().empty(), "error cleared");
		Expect(engine.forwardedOn(20261117) == 1, "count");
		Expect(engine.forwardedOn(20261118) == 0, "other day");
	}

	{
		auto engine = Engine(1);
		engine.addGrouped(7, 9, 1, 1000);
		engine.addGrouped(7, 9, 1, 1100);
		engine.addGrouped(7, 9, 2, 1200);
		engine.addGrouped(8, 9, 3, 1200);
		auto early = engine.takeReadyAlbums(1300, 400);
		Expect(early.empty(), "album quiet");
		auto ready = engine.takeReadyAlbums(1600, 400);
		Expect(ready.size() == 2, "two albums");
		Expect(ready[0].ids.size() == 2, "peer album");
		Expect(ready[1].peerId == 8, "other peer");
		Expect(engine.takeReadyAlbums(2000, 400).empty(), "albums taken");
	}

	{
		auto engine = Engine(1);
		Expect(engine.beginCreate(7), "create");
		Expect(!engine.beginCreate(7), "create race");
		Expect(engine.createPending(7), "pending");
		engine.finishCreate(7, 33);
		Expect(engine.topicFor(7) == 33, "created");
		Expect(engine.peerForTopic(33) == 7, "reverse");
		Expect(!engine.beginCreate(7), "no second create");
		engine.clearTopic(7);
		Expect(engine.beginCreate(7), "create again");
		engine.failCreate(7);
		Expect(!engine.createPending(7), "create failed");
		Expect(engine.beginCreate(7), "retry create");
	}

	{
		auto engine = Engine(1);
		auto settings = Enabled();
		settings.forwardOwn = true;
		engine.setSettings(settings);
		engine.rememberProducedPrivate(7, 4);
		auto message = PrivateText(7, 4);
		message.outgoing = true;
		Expect(
			engine.decideForward(message).skip == Skip::Echo,
			"private produced");
		engine.armEcho(7, 5000);
		message.msgId = 5;
		message.dateUnix = 4000;
		Expect(
			engine.decideForward(message).skip == Skip::Echo,
			"echo window");
		message.dateUnix = 5001;
		Expect(
			engine.decideForward(message).action == Action::Forward,
			"echo expired");
	}

	{
		auto engine = Engine(1);
		Expect(!engine.load("{", 1), "bad json");
		Expect(!engine.settings().enabled, "bad json keeps default");
	}

	if (gFailures) {
		std::cerr << gFailures << " failed\n";
		return 1;
	}
	std::cout << "ok\n";
	return 0;
}
