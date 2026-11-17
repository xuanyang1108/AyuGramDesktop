// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ayu::PrivateForward {

// WHY: messages this subsystem posts carry this invisible prefix so a
// restart can still tell them from a human reply and refuse to bounce them.
inline constexpr char kLoopMarkUtf8[] = "\u2060";

enum class ContentKind : std::uint8_t {
	None,
	Text,
	Photo,
	Video,
	File,
	Voice,
	Sticker,
	Unsupported,
};

enum class Skip : std::uint8_t {
	None,
	Disabled,
	NotPrivate,
	SavedMessages,
	ServiceChat,
	AuthSecret,
	Bot,
	Muted,
	Excluded,
	OwnMessage,
	ProtectedContent,
	Filtered,
	Duplicate,
	Echo,
	Unsupported,
	Empty,
	NoTarget,
	TopicsOff,
	Paused,
	RelayOff,
	NotMappedTopic,
	Forwarded,
	Anonymous,
	NotHuman,
	NeedVerify,
	GroupOnly,
};

enum class Action : std::uint8_t {
	Ignore,
	Forward,
	Relay,
};

struct Settings {
	bool enabled = false;
	std::int64_t targetPeerId = 0;
	bool perPeerTopic = true;
	bool titleWithUsername = true;
	bool allowText = true;
	bool allowPhoto = true;
	bool allowVideo = true;
	bool allowFile = true;
	bool allowVoice = true;
	bool allowSticker = true;
	bool copyWithoutHeader = true;
	bool forwardOwn = true;
	bool editDeleteHints = true;
	bool relayReplies = false;
	bool allowOtherAdmins = false;
	bool ignoreBots = true;
	bool ignoreMuted = false;
	std::vector<std::int64_t> excluded;
	std::int64_t relayEnabledAt = 0;
};

struct Snapshot {
	std::int64_t peerId = 0;
	std::int32_t msgId = 0;
	std::int64_t groupedId = 0;
	std::int64_t dateUnix = 0;
	std::int32_t topicRootId = 0;
	ContentKind kind = ContentKind::None;
	bool privateUser = false;
	bool savedMessages = false;
	bool serviceChat = false;
	bool authSecret = false;
	bool bot = false;
	bool muted = false;
	bool outgoing = false;
	bool protectedContent = false;
	bool serviceMessage = false;
	bool inTargetForum = false;
	bool authorIsSelf = false;
	bool authorIsBot = false;
	bool anonymousAdmin = false;
	bool verifiedAdmin = false;
	bool forwarded = false;
	bool loopMark = false;
};

struct Decision {
	Action action = Action::Ignore;
	Skip skip = Skip::None;
};

struct Album {
	std::int64_t peerId = 0;
	std::int64_t groupedId = 0;
	std::vector<std::int32_t> ids;
};

struct LogRow {
	std::int64_t ts = 0;
	std::string code;
	std::int64_t peerId = 0;
	std::int32_t msgId = 0;
	std::int32_t topicId = 0;
};

class Engine {
public:
	explicit Engine(std::int64_t accountId);

	[[nodiscard]] std::int64_t accountId() const;
	[[nodiscard]] const Settings &settings() const;
	void setSettings(Settings settings);

	void bindTopic(std::int64_t peerId, std::int32_t topicId);
	void clearTopic(std::int64_t peerId);
	[[nodiscard]] std::optional<std::int32_t> topicFor(
		std::int64_t peerId) const;
	[[nodiscard]] std::optional<std::int64_t> peerForTopic(
		std::int32_t topicId) const;

	[[nodiscard]] bool createPending(std::int64_t peerId) const;
	[[nodiscard]] bool beginCreate(std::int64_t peerId);
	void finishCreate(std::int64_t peerId, std::int32_t topicId);
	void failCreate(std::int64_t peerId);

	[[nodiscard]] bool claimForward(
		std::int64_t peerId,
		std::int32_t msgId);
	void releaseForward(std::int64_t peerId, std::int32_t msgId);
	[[nodiscard]] bool wasForwarded(
		std::int64_t peerId,
		std::int32_t msgId) const;

	void rememberProducedForum(std::int32_t msgId);
	void rememberProducedPrivate(
		std::int64_t peerId,
		std::int32_t msgId);
	[[nodiscard]] bool isProducedForum(std::int32_t msgId) const;
	[[nodiscard]] bool isProducedPrivate(
		std::int64_t peerId,
		std::int32_t msgId) const;

	void linkMessages(
		std::int64_t peerId,
		std::int32_t privateMsgId,
		std::int32_t forumMsgId);
	[[nodiscard]] std::optional<std::int32_t> privateForForum(
		std::int32_t forumMsgId,
		std::int64_t peerId) const;
	[[nodiscard]] std::optional<std::int32_t> forumForPrivate(
		std::int64_t peerId,
		std::int32_t privateMsgId) const;

	void rememberRelay(std::int32_t forumMsgId);
	[[nodiscard]] bool wasRelayed(std::int32_t forumMsgId) const;

	[[nodiscard]] bool claimEditHint(
		std::int64_t peerId,
		std::int32_t msgId,
		std::int64_t editDate);
	[[nodiscard]] bool claimDeleteHint(
		std::int64_t peerId,
		std::int32_t msgId);

	void armEcho(std::int64_t peerId, std::int64_t untilUnix);
	[[nodiscard]] bool echoActive(
		std::int64_t peerId,
		std::int64_t dateUnix) const;

	[[nodiscard]] Decision decideForward(const Snapshot &message) const;
	[[nodiscard]] Decision decideRelay(const Snapshot &message) const;

	void addGrouped(
		std::int64_t peerId,
		std::int64_t groupedId,
		std::int32_t msgId,
		std::int64_t nowMs);
	[[nodiscard]] std::vector<Album> takeReadyAlbums(
		std::int64_t nowMs,
		std::int64_t quietMs);

	void noteFlood(int seconds, std::int64_t nowUnix);
	[[nodiscard]] bool floodPaused(std::int64_t nowUnix) const;
	[[nodiscard]] int floodWaitLeft(std::int64_t nowUnix) const;

	void noteForwardSuccess(
		int day,
		std::int64_t nowUnix,
		std::int64_t peerId,
		std::int32_t msgId,
		std::int32_t topicId,
		int count);
	void noteError(
		std::int64_t nowUnix,
		std::string code,
		std::int64_t peerId,
		std::int32_t msgId);
	[[nodiscard]] int forwardedOn(int day) const;
	[[nodiscard]] const std::string &lastError() const;
	[[nodiscard]] const std::vector<LogRow> &log() const;

	void setPaused(std::string reason);
	void clearPaused();
	[[nodiscard]] bool paused() const;
	[[nodiscard]] const std::string &pauseReason() const;
	[[nodiscard]] bool running() const;

	[[nodiscard]] std::string dump() const;
	[[nodiscard]] bool load(
		std::string_view json,
		std::int64_t expectedAccount);

private:
	struct Group {
		std::int64_t peerId = 0;
		std::int64_t groupedId = 0;
		std::int64_t lastMs = 0;
		std::vector<std::int32_t> ids;
	};
	struct Link {
		std::int64_t peerId = 0;
		std::int32_t privateMsgId = 0;
		std::int32_t forumMsgId = 0;
	};

	[[nodiscard]] bool allows(ContentKind kind) const;
	[[nodiscard]] bool excluded(std::int64_t peerId) const;
	void pushLog(LogRow row);
	void rememberKey(
		std::vector<std::string> &order,
		std::vector<std::string> &bag,
		const std::string &key);
	[[nodiscard]] static std::string pairKey(
		std::int64_t peerId,
		std::int32_t msgId);

	std::int64_t _accountId = 0;
	Settings _settings;
	std::vector<std::pair<std::int64_t, std::int32_t>> _topics;
	std::vector<std::int64_t> _creating;
	std::vector<std::string> _forwardOrder;
	std::vector<std::string> _forwarded;
	std::vector<std::int32_t> _producedForum;
	std::vector<std::string> _producedPrivate;
	std::vector<Link> _links;
	std::vector<std::int32_t> _relayed;
	std::vector<std::string> _hintedEdits;
	std::vector<std::string> _hintedDeletes;
	std::vector<std::pair<std::int64_t, std::int64_t>> _echo;
	std::vector<Group> _groups;
	std::int64_t _floodUntil = 0;
	int _day = 0;
	int _dayCount = 0;
	std::string _lastError;
	std::vector<LogRow> _log;
	bool _paused = false;
	std::string _pauseReason;

};

} // namespace Ayu::PrivateForward
