// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/private_forward/forward_policy.h"

#include "ayu/libs/json.hpp"

#include <algorithm>

namespace Ayu::PrivateForward {
namespace {

constexpr auto kCap = 4000;
constexpr auto kLogCap = 200;

[[nodiscard]] bool Contains(
		const std::vector<std::string> &bag,
		const std::string &key) {
	return std::find(bag.begin(), bag.end(), key) != bag.end();
}

[[nodiscard]] bool ContainsId(
		const std::vector<std::int32_t> &bag,
		std::int32_t id) {
	return std::find(bag.begin(), bag.end(), id) != bag.end();
}

void Trim(std::vector<std::string> &order, std::vector<std::string> &bag) {
	while (int(order.size()) > kCap) {
		const auto key = order.front();
		order.erase(order.begin());
		const auto found = std::find(bag.begin(), bag.end(), key);
		if (found != bag.end()) {
			bag.erase(found);
		}
	}
}

[[nodiscard]] Decision Ignore(Skip skip) {
	return Decision{ Action::Ignore, skip };
}

} // namespace

Engine::Engine(std::int64_t accountId)
: _accountId(accountId) {
}

std::int64_t Engine::accountId() const {
	return _accountId;
}

const Settings &Engine::settings() const {
	return _settings;
}

void Engine::setSettings(Settings settings) {
	if (!settings.relayReplies) {
		settings.allowOtherAdmins = false;
	}
	_settings = std::move(settings);
}

void Engine::bindTopic(std::int64_t peerId, std::int32_t topicId) {
	clearTopic(peerId);
	for (auto it = _topics.begin(); it != _topics.end();) {
		if (it->second == topicId) {
			it = _topics.erase(it);
		} else {
			++it;
		}
	}
	_topics.emplace_back(peerId, topicId);
}

void Engine::clearTopic(std::int64_t peerId) {
	_topics.erase(
		std::remove_if(
			_topics.begin(),
			_topics.end(),
			[&](const auto &row) { return row.first == peerId; }),
		_topics.end());
}

std::optional<std::int32_t> Engine::topicFor(std::int64_t peerId) const {
	for (const auto &row : _topics) {
		if (row.first == peerId) {
			return row.second;
		}
	}
	return std::nullopt;
}

std::optional<std::int64_t> Engine::peerForTopic(std::int32_t topicId) const {
	for (const auto &row : _topics) {
		if (row.second == topicId) {
			return row.first;
		}
	}
	return std::nullopt;
}

bool Engine::createPending(std::int64_t peerId) const {
	return std::find(_creating.begin(), _creating.end(), peerId)
		!= _creating.end();
}

bool Engine::beginCreate(std::int64_t peerId) {
	if (topicFor(peerId) || createPending(peerId)) {
		return false;
	}
	_creating.push_back(peerId);
	return true;
}

void Engine::finishCreate(std::int64_t peerId, std::int32_t topicId) {
	failCreate(peerId);
	if (topicId > 0) {
		bindTopic(peerId, topicId);
	}
}

void Engine::failCreate(std::int64_t peerId) {
	_creating.erase(
		std::remove(_creating.begin(), _creating.end(), peerId),
		_creating.end());
}

std::string Engine::pairKey(std::int64_t peerId, std::int32_t msgId) {
	return std::to_string(peerId) + ':' + std::to_string(msgId);
}

void Engine::rememberKey(
		std::vector<std::string> &order,
		std::vector<std::string> &bag,
		const std::string &key) {
	if (Contains(bag, key)) {
		return;
	}
	bag.push_back(key);
	order.push_back(key);
	Trim(order, bag);
}

bool Engine::claimForward(std::int64_t peerId, std::int32_t msgId) {
	const auto key = pairKey(peerId, msgId);
	if (Contains(_forwarded, key)) {
		return false;
	}
	rememberKey(_forwardOrder, _forwarded, key);
	return true;
}

void Engine::releaseForward(std::int64_t peerId, std::int32_t msgId) {
	const auto key = pairKey(peerId, msgId);
	_forwarded.erase(
		std::remove(_forwarded.begin(), _forwarded.end(), key),
		_forwarded.end());
	_forwardOrder.erase(
		std::remove(_forwardOrder.begin(), _forwardOrder.end(), key),
		_forwardOrder.end());
}

bool Engine::wasForwarded(std::int64_t peerId, std::int32_t msgId) const {
	return Contains(_forwarded, pairKey(peerId, msgId));
}

void Engine::rememberProducedForum(std::int32_t msgId) {
	if (msgId <= 0 || ContainsId(_producedForum, msgId)) {
		return;
	}
	_producedForum.push_back(msgId);
	while (int(_producedForum.size()) > kCap) {
		_producedForum.erase(_producedForum.begin());
	}
}

void Engine::rememberProducedPrivate(
		std::int64_t peerId,
		std::int32_t msgId) {
	const auto key = pairKey(peerId, msgId);
	if (Contains(_producedPrivate, key)) {
		return;
	}
	_producedPrivate.push_back(key);
	while (int(_producedPrivate.size()) > kCap) {
		_producedPrivate.erase(_producedPrivate.begin());
	}
}

bool Engine::isProducedForum(std::int32_t msgId) const {
	return ContainsId(_producedForum, msgId);
}

bool Engine::isProducedPrivate(
		std::int64_t peerId,
		std::int32_t msgId) const {
	return Contains(_producedPrivate, pairKey(peerId, msgId));
}

void Engine::linkMessages(
		std::int64_t peerId,
		std::int32_t privateMsgId,
		std::int32_t forumMsgId) {
	if (privateMsgId <= 0 || forumMsgId <= 0) {
		return;
	}
	for (auto &link : _links) {
		if (link.forumMsgId == forumMsgId
			|| (link.peerId == peerId && link.privateMsgId == privateMsgId)) {
			link.peerId = peerId;
			link.privateMsgId = privateMsgId;
			link.forumMsgId = forumMsgId;
			return;
		}
	}
	_links.push_back({ peerId, privateMsgId, forumMsgId });
	while (int(_links.size()) > kCap) {
		_links.erase(_links.begin());
	}
}

std::optional<std::int32_t> Engine::privateForForum(
		std::int32_t forumMsgId,
		std::int64_t peerId) const {
	for (const auto &link : _links) {
		if (link.forumMsgId == forumMsgId && link.peerId == peerId) {
			return link.privateMsgId;
		}
	}
	return std::nullopt;
}

std::optional<std::int32_t> Engine::forumForPrivate(
		std::int64_t peerId,
		std::int32_t privateMsgId) const {
	for (const auto &link : _links) {
		if (link.peerId == peerId && link.privateMsgId == privateMsgId) {
			return link.forumMsgId;
		}
	}
	return std::nullopt;
}

void Engine::rememberRelay(std::int32_t forumMsgId) {
	if (forumMsgId <= 0 || ContainsId(_relayed, forumMsgId)) {
		return;
	}
	_relayed.push_back(forumMsgId);
	while (int(_relayed.size()) > kCap) {
		_relayed.erase(_relayed.begin());
	}
}

bool Engine::wasRelayed(std::int32_t forumMsgId) const {
	return ContainsId(_relayed, forumMsgId);
}

bool Engine::claimEditHint(
		std::int64_t peerId,
		std::int32_t msgId,
		std::int64_t editDate) {
	const auto key = pairKey(peerId, msgId) + ':' + std::to_string(editDate);
	if (Contains(_hintedEdits, key)) {
		return false;
	}
	_hintedEdits.push_back(key);
	while (int(_hintedEdits.size()) > kCap) {
		_hintedEdits.erase(_hintedEdits.begin());
	}
	return true;
}

bool Engine::claimDeleteHint(std::int64_t peerId, std::int32_t msgId) {
	const auto key = pairKey(peerId, msgId);
	if (Contains(_hintedDeletes, key)) {
		return false;
	}
	_hintedDeletes.push_back(key);
	while (int(_hintedDeletes.size()) > kCap) {
		_hintedDeletes.erase(_hintedDeletes.begin());
	}
	return true;
}

void Engine::armEcho(std::int64_t peerId, std::int64_t untilUnix) {
	for (auto &row : _echo) {
		if (row.first == peerId) {
			row.second = std::max(row.second, untilUnix);
			return;
		}
	}
	_echo.emplace_back(peerId, untilUnix);
}

bool Engine::echoActive(std::int64_t peerId, std::int64_t dateUnix) const {
	for (const auto &row : _echo) {
		if (row.first == peerId) {
			return dateUnix <= row.second;
		}
	}
	return false;
}

bool Engine::allows(ContentKind kind) const {
	switch (kind) {
	case ContentKind::Text: return _settings.allowText;
	case ContentKind::Photo: return _settings.allowPhoto;
	case ContentKind::Video: return _settings.allowVideo;
	case ContentKind::File: return _settings.allowFile;
	case ContentKind::Voice: return _settings.allowVoice;
	case ContentKind::Sticker: return _settings.allowSticker;
	case ContentKind::None:
	case ContentKind::Unsupported: return false;
	}
	return false;
}

bool Engine::excluded(std::int64_t peerId) const {
	return std::find(
		_settings.excluded.begin(),
		_settings.excluded.end(),
		peerId) != _settings.excluded.end();
}

Decision Engine::decideForward(const Snapshot &message) const {
	if (!_settings.enabled) {
		return Ignore(Skip::Disabled);
	}
	if (_paused) {
		return Ignore(Skip::Paused);
	}
	if (!_settings.targetPeerId) {
		return Ignore(Skip::NoTarget);
	}
	if (!_settings.perPeerTopic) {
		return Ignore(Skip::TopicsOff);
	}
	if (message.savedMessages) {
		return Ignore(Skip::SavedMessages);
	}
	if (message.serviceChat) {
		return Ignore(Skip::ServiceChat);
	}
	if (message.authSecret) {
		return Ignore(Skip::AuthSecret);
	}
	if (!message.privateUser || message.inTargetForum) {
		return Ignore(Skip::NotPrivate);
	}
	if (message.serviceMessage) {
		return Ignore(Skip::Unsupported);
	}
	if (message.loopMark
		|| isProducedPrivate(message.peerId, message.msgId)) {
		return Ignore(Skip::Echo);
	}
	if (message.outgoing
		&& echoActive(message.peerId, message.dateUnix)) {
		return Ignore(Skip::Echo);
	}
	if (message.protectedContent) {
		return Ignore(Skip::ProtectedContent);
	}
	if (message.bot && _settings.ignoreBots) {
		return Ignore(Skip::Bot);
	}
	if (message.muted && _settings.ignoreMuted) {
		return Ignore(Skip::Muted);
	}
	if (excluded(message.peerId)) {
		return Ignore(Skip::Excluded);
	}
	if (message.outgoing && !_settings.forwardOwn) {
		return Ignore(Skip::OwnMessage);
	}
	if (wasForwarded(message.peerId, message.msgId)) {
		return Ignore(Skip::Duplicate);
	}
	if (message.kind == ContentKind::None) {
		return Ignore(Skip::Empty);
	}
	if (message.kind == ContentKind::Unsupported || !allows(message.kind)) {
		return Ignore(
			message.kind == ContentKind::Unsupported
				? Skip::Unsupported
				: Skip::Filtered);
	}
	return Decision{ Action::Forward, Skip::None };
}

Decision Engine::decideRelay(const Snapshot &message) const {
	if (!_settings.enabled || !_settings.relayReplies) {
		return Ignore(Skip::RelayOff);
	}
	if (_paused) {
		return Ignore(Skip::Paused);
	}
	if (!message.inTargetForum || !message.topicRootId) {
		return Ignore(Skip::NotMappedTopic);
	}
	if (!peerForTopic(message.topicRootId)) {
		return Ignore(Skip::NotMappedTopic);
	}
	if (message.serviceMessage) {
		return Ignore(Skip::Unsupported);
	}
	if (message.forwarded) {
		return Ignore(Skip::Forwarded);
	}
	if (message.loopMark
		|| isProducedForum(message.msgId)
		|| wasRelayed(message.msgId)) {
		return Ignore(Skip::Echo);
	}
	if (message.anonymousAdmin) {
		return Ignore(Skip::Anonymous);
	}
	if (message.authorIsBot) {
		return Ignore(Skip::NotHuman);
	}
	if (message.kind == ContentKind::None) {
		return Ignore(Skip::Empty);
	}
	if (message.kind == ContentKind::Unsupported) {
		return Ignore(Skip::Unsupported);
	}
	if (message.authorIsSelf) {
		return Decision{ Action::Relay, Skip::None };
	}
	if (!_settings.allowOtherAdmins) {
		return Ignore(Skip::GroupOnly);
	}
	if (!message.verifiedAdmin) {
		return Ignore(Skip::NeedVerify);
	}
	return Decision{ Action::Relay, Skip::None };
}

void Engine::addGrouped(
		std::int64_t peerId,
		std::int64_t groupedId,
		std::int32_t msgId,
		std::int64_t nowMs) {
	if (!groupedId || msgId <= 0) {
		return;
	}
	for (auto &group : _groups) {
		if (group.peerId == peerId && group.groupedId == groupedId) {
			if (!ContainsId(group.ids, msgId)) {
				group.ids.push_back(msgId);
			}
			group.lastMs = nowMs;
			return;
		}
	}
	_groups.push_back({ peerId, groupedId, nowMs, { msgId } });
}

std::vector<Album> Engine::takeReadyAlbums(
		std::int64_t nowMs,
		std::int64_t quietMs) {
	auto ready = std::vector<Album>();
	auto stay = std::vector<Group>();
	for (auto &group : _groups) {
		if (nowMs - group.lastMs >= quietMs) {
			ready.push_back({
				group.peerId,
				group.groupedId,
				std::move(group.ids),
			});
		} else {
			stay.push_back(std::move(group));
		}
	}
	_groups = std::move(stay);
	return ready;
}

void Engine::noteFlood(int seconds, std::int64_t nowUnix) {
	if (seconds < 1) {
		seconds = 1;
	}
	_floodUntil = std::max(_floodUntil, nowUnix + seconds);
}

bool Engine::floodPaused(std::int64_t nowUnix) const {
	return nowUnix < _floodUntil;
}

int Engine::floodWaitLeft(std::int64_t nowUnix) const {
	if (nowUnix >= _floodUntil) {
		return 0;
	}
	return int(_floodUntil - nowUnix);
}

void Engine::pushLog(LogRow row) {
	_log.push_back(std::move(row));
	while (int(_log.size()) > kLogCap) {
		_log.erase(_log.begin());
	}
}

void Engine::noteForwardSuccess(
		int day,
		std::int64_t nowUnix,
		std::int64_t peerId,
		std::int32_t msgId,
		std::int32_t topicId,
		int count) {
	if (_day != day) {
		_day = day;
		_dayCount = 0;
	}
	if (count < 1) {
		count = 1;
	}
	_dayCount += count;
	_lastError.clear();
	pushLog({ nowUnix, "forwarded", peerId, msgId, topicId });
}

void Engine::noteError(
		std::int64_t nowUnix,
		std::string code,
		std::int64_t peerId,
		std::int32_t msgId) {
	_lastError = code;
	pushLog({ nowUnix, std::move(code), peerId, msgId, 0 });
}

int Engine::forwardedOn(int day) const {
	return (_day == day) ? _dayCount : 0;
}

const std::string &Engine::lastError() const {
	return _lastError;
}

const std::vector<LogRow> &Engine::log() const {
	return _log;
}

void Engine::setPaused(std::string reason) {
	_paused = true;
	_pauseReason = std::move(reason);
}

void Engine::clearPaused() {
	_paused = false;
	_pauseReason.clear();
}

bool Engine::paused() const {
	return _paused;
}

const std::string &Engine::pauseReason() const {
	return _pauseReason;
}

bool Engine::running() const {
	return _settings.enabled
		&& _settings.targetPeerId != 0
		&& _settings.perPeerTopic
		&& !_paused;
}

std::string Engine::dump() const {
	auto topics = nlohmann::json::array();
	for (const auto &row : _topics) {
		topics.push_back({
			{ "peer", row.first },
			{ "topic", row.second },
		});
	}
	auto links = nlohmann::json::array();
	for (const auto &link : _links) {
		links.push_back({
			{ "peer", link.peerId },
			{ "msg", link.privateMsgId },
			{ "forum", link.forumMsgId },
		});
	}
	auto echo = nlohmann::json::array();
	for (const auto &row : _echo) {
		echo.push_back({
			{ "peer", row.first },
			{ "until", row.second },
		});
	}
	auto log = nlohmann::json::array();
	for (const auto &row : _log) {
		log.push_back({
			{ "ts", row.ts },
			{ "code", row.code },
			{ "peer", row.peerId },
			{ "msg", row.msgId },
			{ "topic", row.topicId },
		});
	}
	const auto json = nlohmann::json{
		{ "version", 1 },
		{ "accountId", _accountId },
		{ "settings", {
			{ "enabled", _settings.enabled },
			{ "targetPeerId", _settings.targetPeerId },
			{ "perPeerTopic", _settings.perPeerTopic },
			{ "titleWithUsername", _settings.titleWithUsername },
			{ "allowText", _settings.allowText },
			{ "allowPhoto", _settings.allowPhoto },
			{ "allowVideo", _settings.allowVideo },
			{ "allowFile", _settings.allowFile },
			{ "allowVoice", _settings.allowVoice },
			{ "allowSticker", _settings.allowSticker },
			{ "copyWithoutHeader", _settings.copyWithoutHeader },
			{ "forwardOwn", _settings.forwardOwn },
			{ "editDeleteHints", _settings.editDeleteHints },
			{ "relayReplies", _settings.relayReplies },
			{ "allowOtherAdmins", _settings.allowOtherAdmins },
			{ "ignoreBots", _settings.ignoreBots },
			{ "ignoreMuted", _settings.ignoreMuted },
			{ "excluded", _settings.excluded },
			{ "relayEnabledAt", _settings.relayEnabledAt },
		} },
		{ "topics", topics },
		{ "forwarded", _forwarded },
		{ "producedForum", _producedForum },
		{ "producedPrivate", _producedPrivate },
		{ "links", links },
		{ "relayed", _relayed },
		{ "hintedEdits", _hintedEdits },
		{ "hintedDeletes", _hintedDeletes },
		{ "echo", echo },
		{ "day", _day },
		{ "dayCount", _dayCount },
		{ "lastError", _lastError },
		{ "log", log },
	};
	return json.dump();
}

bool Engine::load(std::string_view json, std::int64_t expectedAccount) {
	try {
		const auto parsed = nlohmann::json::parse(json);
		if (!parsed.is_object()) {
			return false;
		}
		if (parsed.value("accountId", std::int64_t(0)) != expectedAccount) {
			return false;
		}
		const auto &settings = parsed.at("settings");
		auto next = Settings();
		next.enabled = settings.value("enabled", false);
		next.targetPeerId = settings.value("targetPeerId", std::int64_t(0));
		next.perPeerTopic = settings.value("perPeerTopic", true);
		next.titleWithUsername = settings.value("titleWithUsername", true);
		next.allowText = settings.value("allowText", true);
		next.allowPhoto = settings.value("allowPhoto", true);
		next.allowVideo = settings.value("allowVideo", true);
		next.allowFile = settings.value("allowFile", true);
		next.allowVoice = settings.value("allowVoice", true);
		next.allowSticker = settings.value("allowSticker", true);
		next.copyWithoutHeader = settings.value("copyWithoutHeader", true);
		next.forwardOwn = settings.value("forwardOwn", true);
		next.editDeleteHints = settings.value("editDeleteHints", true);
		next.relayReplies = settings.value("relayReplies", false);
		next.allowOtherAdmins = settings.value("allowOtherAdmins", false);
		next.ignoreBots = settings.value("ignoreBots", true);
		next.ignoreMuted = settings.value("ignoreMuted", false);
		next.excluded = settings.value(
			"excluded",
			std::vector<std::int64_t>());
		next.relayEnabledAt = settings.value(
			"relayEnabledAt",
			std::int64_t(0));
		if (!next.relayReplies) {
			next.allowOtherAdmins = false;
		}

		auto topics = std::vector<std::pair<std::int64_t, std::int32_t>>();
		for (const auto &row : parsed.value("topics", nlohmann::json::array())) {
			topics.emplace_back(
				row.value("peer", std::int64_t(0)),
				row.value("topic", std::int32_t(0)));
		}
		auto links = std::vector<Link>();
		for (const auto &row : parsed.value("links", nlohmann::json::array())) {
			links.push_back({
				row.value("peer", std::int64_t(0)),
				row.value("msg", std::int32_t(0)),
				row.value("forum", std::int32_t(0)),
			});
		}
		auto echo = std::vector<std::pair<std::int64_t, std::int64_t>>();
		for (const auto &row : parsed.value("echo", nlohmann::json::array())) {
			echo.emplace_back(
				row.value("peer", std::int64_t(0)),
				row.value("until", std::int64_t(0)));
		}
		auto log = std::vector<LogRow>();
		for (const auto &row : parsed.value("log", nlohmann::json::array())) {
			log.push_back({
				row.value("ts", std::int64_t(0)),
				row.value("code", std::string()),
				row.value("peer", std::int64_t(0)),
				row.value("msg", std::int32_t(0)),
				row.value("topic", std::int32_t(0)),
			});
		}

		_settings = std::move(next);
		_topics = std::move(topics);
		_creating.clear();
		_forwarded = parsed.value("forwarded", std::vector<std::string>());
		_forwardOrder = _forwarded;
		_producedForum = parsed.value(
			"producedForum",
			std::vector<std::int32_t>());
		_producedPrivate = parsed.value(
			"producedPrivate",
			std::vector<std::string>());
		_links = std::move(links);
		_relayed = parsed.value("relayed", std::vector<std::int32_t>());
		_hintedEdits = parsed.value("hintedEdits", std::vector<std::string>());
		_hintedDeletes = parsed.value(
			"hintedDeletes",
			std::vector<std::string>());
		_echo = std::move(echo);
		_groups.clear();
		_floodUntil = 0;
		_day = parsed.value("day", 0);
		_dayCount = parsed.value("dayCount", 0);
		_lastError = parsed.value("lastError", std::string());
		_log = std::move(log);
		_paused = false;
		_pauseReason.clear();
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace Ayu::PrivateForward
