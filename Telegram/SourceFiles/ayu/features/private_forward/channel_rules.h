// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ayu::PrivateForward {

// WHY: enabledAt is the server-time cutoff for one rule. Older updates that
// arrive after a restart are not history to backfill, so they are ignored.
enum class PeerKind : std::uint8_t {
	Other,
	User,
	Group,
	Broadcast,
	Forum,
};

enum class RuleError : std::uint8_t {
	None,
	SamePeer,
	SourceNotBroadcast,
	TargetNotSendable,
	TopicRequired,
	TopicUnexpected,
	Duplicate,
	Missing,
};

enum class ChannelSkip : std::uint8_t {
	None,
	GlobalOff,
	NotBroadcast,
	RuleOff,
	BeforeEnable,
	Protected,
	Service,
	Duplicate,
	Loop,
	SamePeer,
	NoPermission,
	Paused,
	Deleted,
};

struct ChannelRule {
	std::int64_t id = 0;
	std::int64_t sourcePeerId = 0;
	std::int64_t targetPeerId = 0;
	std::int32_t topicId = 0;
	bool enabled = false;
	std::int64_t enabledAt = 0;
	bool paused = false;
	std::string pauseReason;
};

struct ChannelMessage {
	std::int64_t peerId = 0;
	std::int32_t msgId = 0;
	std::int64_t groupedId = 0;
	std::int64_t dateUnix = 0;
	bool broadcast = false;
	bool service = false;
	bool protectedContent = false;
	bool forwarded = false;
	std::int64_t forwardFromPeer = 0;
	std::int32_t forwardFromMsg = 0;
	bool sourceAccessible = true;
};

struct Route {
	bool forward = false;
	ChannelSkip skip = ChannelSkip::None;
	std::int64_t ruleId = 0;
	std::int64_t targetPeerId = 0;
	std::int32_t topicId = 0;
};

struct ChannelAlbum {
	std::int64_t ruleId = 0;
	std::int64_t peerId = 0;
	std::int64_t groupedId = 0;
	std::int64_t targetPeerId = 0;
	std::int32_t topicId = 0;
	std::vector<std::int32_t> ids;
};

struct ChannelLogRow {
	std::int64_t ts = 0;
	std::string code;
	std::int64_t peerId = 0;
	std::int32_t msgId = 0;
	std::int64_t targetPeerId = 0;
};

class ChannelRules {
public:
	explicit ChannelRules(std::int64_t accountId);

	[[nodiscard]] std::int64_t accountId() const;
	[[nodiscard]] bool globalEnabled() const;
	void setGlobalEnabled(bool enabled, std::int64_t nowUnix);

	[[nodiscard]] const std::vector<ChannelRule> &rules() const;
	[[nodiscard]] const ChannelRule *rule(std::int64_t id) const;

	[[nodiscard]] RuleError addRule(
		ChannelRule rule,
		PeerKind sourceKind,
		PeerKind targetKind,
		bool targetCanSend);
	[[nodiscard]] RuleError updateRule(
		ChannelRule rule,
		PeerKind sourceKind,
		PeerKind targetKind,
		bool targetCanSend);
	void removeRule(std::int64_t id);
	[[nodiscard]] RuleError setRuleEnabled(
		std::int64_t id,
		bool enabled,
		std::int64_t nowUnix);
	void pauseRule(std::int64_t id, std::string reason);

	[[nodiscard]] std::vector<Route> route(
		const ChannelMessage &message,
		const std::function<bool(std::int64_t)> &targetCanSend) const;

	[[nodiscard]] bool claim(
		std::int64_t ruleId,
		std::int64_t sourcePeer,
		std::int32_t msgId);
	void release(
		std::int64_t ruleId,
		std::int64_t sourcePeer,
		std::int32_t msgId);
	void rememberProduced(std::int64_t targetPeer, std::int32_t msgId);
	[[nodiscard]] bool isProduced(
		std::int64_t peerId,
		std::int32_t msgId) const;
	[[nodiscard]] bool originTaken(
		std::int64_t sourcePeer,
		std::int32_t msgId) const;

	void addGrouped(const Route &route, const ChannelMessage &message, std::int64_t nowMs);
	[[nodiscard]] std::vector<ChannelAlbum> takeReadyAlbums(
		std::int64_t nowMs,
		std::int64_t quietMs);

	void noteSuccess(
		int day,
		std::int64_t nowUnix,
		std::int64_t sourcePeer,
		std::int32_t msgId,
		std::int64_t targetPeer,
		int count);
	void noteError(
		std::int64_t nowUnix,
		std::string code,
		std::int64_t peerId,
		std::int32_t msgId);
	[[nodiscard]] int forwardedOn(int day) const;
	[[nodiscard]] const std::string &lastError() const;
	[[nodiscard]] const std::vector<ChannelLogRow> &log() const;

	[[nodiscard]] std::string dump() const;
	[[nodiscard]] bool load(std::string_view json, std::int64_t expectedAccount);

private:
	struct Group {
		std::int64_t ruleId = 0;
		std::int64_t peerId = 0;
		std::int64_t groupedId = 0;
		std::int64_t targetPeerId = 0;
		std::int32_t topicId = 0;
		std::int64_t lastMs = 0;
		std::vector<std::int32_t> ids;
	};

	[[nodiscard]] RuleError validate(
		const ChannelRule &rule,
		PeerKind sourceKind,
		PeerKind targetKind,
		bool targetCanSend,
		std::int64_t ignoreId) const;
	[[nodiscard]] static std::string claimKey(
		std::int64_t ruleId,
		std::int64_t peerId,
		std::int32_t msgId);
	[[nodiscard]] static std::string pairKey(
		std::int64_t peerId,
		std::int32_t msgId);

	std::int64_t _accountId = 0;
	bool _global = false;
	std::int64_t _nextId = 1;
	std::vector<ChannelRule> _rules;
	std::vector<std::string> _claims;
	std::vector<std::string> _origins;
	std::vector<std::string> _produced;
	std::vector<Group> _groups;
	int _day = 0;
	int _dayCount = 0;
	std::string _lastError;
	std::vector<ChannelLogRow> _log;

};

} // namespace Ayu::PrivateForward
