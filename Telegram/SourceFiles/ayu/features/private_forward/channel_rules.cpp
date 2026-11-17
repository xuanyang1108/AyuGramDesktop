// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/private_forward/channel_rules.h"

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

void Remember(std::vector<std::string> &bag, const std::string &key) {
	if (Contains(bag, key)) {
		return;
	}
	bag.push_back(key);
	while (int(bag.size()) > kCap) {
		bag.erase(bag.begin());
	}
}

} // namespace

ChannelRules::ChannelRules(std::int64_t accountId)
: _accountId(accountId) {
}

std::int64_t ChannelRules::accountId() const {
	return _accountId;
}

bool ChannelRules::globalEnabled() const {
	return _global;
}

void ChannelRules::setGlobalEnabled(bool enabled, std::int64_t nowUnix) {
	_global = enabled;
	if (!enabled) {
		return;
	}
	for (auto &rule : _rules) {
		if (rule.enabled) {
			rule.enabledAt = nowUnix;
			rule.paused = false;
			rule.pauseReason.clear();
		}
	}
}

const std::vector<ChannelRule> &ChannelRules::rules() const {
	return _rules;
}

const ChannelRule *ChannelRules::rule(std::int64_t id) const {
	for (const auto &rule : _rules) {
		if (rule.id == id) {
			return &rule;
		}
	}
	return nullptr;
}

RuleError ChannelRules::validate(
		const ChannelRule &rule,
		PeerKind sourceKind,
		PeerKind targetKind,
		bool targetCanSend,
		std::int64_t ignoreId) const {
	if (!rule.sourcePeerId || !rule.targetPeerId) {
		return RuleError::TargetNotSendable;
	}
	if (rule.sourcePeerId == rule.targetPeerId) {
		return RuleError::SamePeer;
	}
	if (sourceKind != PeerKind::Broadcast) {
		return RuleError::SourceNotBroadcast;
	}
	const auto targetOk = targetKind == PeerKind::Group
		|| targetKind == PeerKind::Forum
		|| targetKind == PeerKind::Broadcast;
	if (!targetOk || !targetCanSend) {
		return RuleError::TargetNotSendable;
	}
	if (targetKind == PeerKind::Forum) {
		if (rule.topicId <= 0) {
			return RuleError::TopicRequired;
		}
	} else if (rule.topicId != 0) {
		return RuleError::TopicUnexpected;
	}
	for (const auto &other : _rules) {
		if (other.id == ignoreId) {
			continue;
		}
		if (other.sourcePeerId == rule.sourcePeerId
			&& other.targetPeerId == rule.targetPeerId
			&& other.topicId == rule.topicId) {
			return RuleError::Duplicate;
		}
	}
	return RuleError::None;
}

RuleError ChannelRules::addRule(
		ChannelRule rule,
		PeerKind sourceKind,
		PeerKind targetKind,
		bool targetCanSend) {
	const auto error = validate(
		rule,
		sourceKind,
		targetKind,
		targetCanSend,
		0);
	if (error != RuleError::None) {
		return error;
	}
	rule.id = _nextId++;
	rule.enabled = false;
	rule.enabledAt = 0;
	rule.paused = false;
	rule.pauseReason.clear();
	_rules.push_back(std::move(rule));
	return RuleError::None;
}

RuleError ChannelRules::updateRule(
		ChannelRule rule,
		PeerKind sourceKind,
		PeerKind targetKind,
		bool targetCanSend) {
	const auto error = validate(
		rule,
		sourceKind,
		targetKind,
		targetCanSend,
		rule.id);
	if (error != RuleError::None) {
		return error;
	}
	for (auto &other : _rules) {
		if (other.id != rule.id) {
			continue;
		}
		const auto wasEnabled = other.enabled;
		const auto enabledAt = other.enabledAt;
		other.sourcePeerId = rule.sourcePeerId;
		other.targetPeerId = rule.targetPeerId;
		other.topicId = rule.topicId;
		other.enabled = wasEnabled;
		other.enabledAt = enabledAt;
		other.paused = false;
		other.pauseReason.clear();
		return RuleError::None;
	}
	return RuleError::Missing;
}

void ChannelRules::removeRule(std::int64_t id) {
	_rules.erase(
		std::remove_if(
			_rules.begin(),
			_rules.end(),
			[&](const ChannelRule &rule) { return rule.id == id; }),
		_rules.end());
}

RuleError ChannelRules::setRuleEnabled(
		std::int64_t id,
		bool enabled,
		std::int64_t nowUnix) {
	for (auto &rule : _rules) {
		if (rule.id != id) {
			continue;
		}
		if (enabled && nowUnix <= 0) {
			return RuleError::Missing;
		}
		rule.enabled = enabled;
		if (enabled) {
			rule.enabledAt = nowUnix;
			rule.paused = false;
			rule.pauseReason.clear();
		}
		return RuleError::None;
	}
	return RuleError::Missing;
}

void ChannelRules::pauseRule(std::int64_t id, std::string reason) {
	for (auto &rule : _rules) {
		if (rule.id == id) {
			rule.paused = true;
			rule.pauseReason = std::move(reason);
			return;
		}
	}
}

std::string ChannelRules::claimKey(
		std::int64_t ruleId,
		std::int64_t peerId,
		std::int32_t msgId) {
	return std::to_string(ruleId)
		+ ':'
		+ std::to_string(peerId)
		+ ':'
		+ std::to_string(msgId);
}

std::string ChannelRules::pairKey(std::int64_t peerId, std::int32_t msgId) {
	return std::to_string(peerId) + ':' + std::to_string(msgId);
}

std::vector<Route> ChannelRules::route(
		const ChannelMessage &message,
		const std::function<bool(std::int64_t)> &targetCanSend) const {
	if (!_global) {
		return { Route{ false, ChannelSkip::GlobalOff, 0, 0, 0 } };
	}
	if (!message.broadcast) {
		return { Route{ false, ChannelSkip::NotBroadcast, 0, 0, 0 } };
	}
	if (!message.sourceAccessible) {
		return { Route{ false, ChannelSkip::Deleted, 0, 0, 0 } };
	}
	if (message.service) {
		return { Route{ false, ChannelSkip::Service, 0, 0, 0 } };
	}
	if (message.protectedContent) {
		return { Route{ false, ChannelSkip::Protected, 0, 0, 0 } };
	}
	if (isProduced(message.peerId, message.msgId)) {
		return { Route{ false, ChannelSkip::Loop, 0, 0, 0 } };
	}
	if (message.forwarded
		&& (originTaken(message.forwardFromPeer, message.forwardFromMsg)
			|| isProduced(message.forwardFromPeer, message.forwardFromMsg))) {
		return { Route{ false, ChannelSkip::Loop, 0, 0, 0 } };
	}
	auto routes = std::vector<Route>();
	for (const auto &rule : _rules) {
		if (rule.sourcePeerId != message.peerId) {
			continue;
		}
		if (!rule.enabled) {
			routes.push_back({
				false,
				ChannelSkip::RuleOff,
				rule.id,
				rule.targetPeerId,
				rule.topicId,
			});
			continue;
		}
		if (rule.paused) {
			routes.push_back({
				false,
				ChannelSkip::Paused,
				rule.id,
				rule.targetPeerId,
				rule.topicId,
			});
			continue;
		}
		if (rule.sourcePeerId == rule.targetPeerId) {
			routes.push_back({
				false,
				ChannelSkip::SamePeer,
				rule.id,
				rule.targetPeerId,
				rule.topicId,
			});
			continue;
		}
		if (rule.enabledAt <= 0 || message.dateUnix < rule.enabledAt) {
			routes.push_back({
				false,
				ChannelSkip::BeforeEnable,
				rule.id,
				rule.targetPeerId,
				rule.topicId,
			});
			continue;
		}
		if (!targetCanSend || !targetCanSend(rule.targetPeerId)) {
			routes.push_back({
				false,
				ChannelSkip::NoPermission,
				rule.id,
				rule.targetPeerId,
				rule.topicId,
			});
			continue;
		}
		const auto key = claimKey(rule.id, message.peerId, message.msgId);
		if (Contains(_claims, key)) {
			routes.push_back({
				false,
				ChannelSkip::Duplicate,
				rule.id,
				rule.targetPeerId,
				rule.topicId,
			});
			continue;
		}
		routes.push_back({
			true,
			ChannelSkip::None,
			rule.id,
			rule.targetPeerId,
			rule.topicId,
		});
	}
	return routes;
}

bool ChannelRules::claim(
		std::int64_t ruleId,
		std::int64_t sourcePeer,
		std::int32_t msgId) {
	const auto key = claimKey(ruleId, sourcePeer, msgId);
	if (Contains(_claims, key)) {
		return false;
	}
	Remember(_claims, key);
	Remember(_origins, pairKey(sourcePeer, msgId));
	return true;
}

void ChannelRules::release(
		std::int64_t ruleId,
		std::int64_t sourcePeer,
		std::int32_t msgId) {
	const auto key = claimKey(ruleId, sourcePeer, msgId);
	_claims.erase(
		std::remove(_claims.begin(), _claims.end(), key),
		_claims.end());
}

void ChannelRules::rememberProduced(
		std::int64_t targetPeer,
		std::int32_t msgId) {
	Remember(_produced, pairKey(targetPeer, msgId));
}

bool ChannelRules::isProduced(
		std::int64_t peerId,
		std::int32_t msgId) const {
	return Contains(_produced, pairKey(peerId, msgId));
}

bool ChannelRules::originTaken(
		std::int64_t sourcePeer,
		std::int32_t msgId) const {
	return Contains(_origins, pairKey(sourcePeer, msgId));
}

void ChannelRules::addGrouped(
		const Route &route,
		const ChannelMessage &message,
		std::int64_t nowMs) {
	if (!route.forward || !message.groupedId) {
		return;
	}
	for (auto &group : _groups) {
		if (group.ruleId == route.ruleId
			&& group.peerId == message.peerId
			&& group.groupedId == message.groupedId) {
			if (std::find(group.ids.begin(), group.ids.end(), message.msgId)
				== group.ids.end()) {
				group.ids.push_back(message.msgId);
			}
			group.lastMs = nowMs;
			return;
		}
	}
	_groups.push_back({
		route.ruleId,
		message.peerId,
		message.groupedId,
		route.targetPeerId,
		route.topicId,
		nowMs,
		{ message.msgId },
	});
}

std::vector<ChannelAlbum> ChannelRules::takeReadyAlbums(
		std::int64_t nowMs,
		std::int64_t quietMs) {
	auto ready = std::vector<ChannelAlbum>();
	auto stay = std::vector<Group>();
	for (auto &group : _groups) {
		if (nowMs - group.lastMs >= quietMs) {
			ready.push_back({
				group.ruleId,
				group.peerId,
				group.groupedId,
				group.targetPeerId,
				group.topicId,
				std::move(group.ids),
			});
		} else {
			stay.push_back(std::move(group));
		}
	}
	_groups = std::move(stay);
	return ready;
}

void ChannelRules::noteSuccess(
		int day,
		std::int64_t nowUnix,
		std::int64_t sourcePeer,
		std::int32_t msgId,
		std::int64_t targetPeer,
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
	_log.push_back({ nowUnix, "forwarded", sourcePeer, msgId, targetPeer });
	while (int(_log.size()) > kLogCap) {
		_log.erase(_log.begin());
	}
}

void ChannelRules::noteError(
		std::int64_t nowUnix,
		std::string code,
		std::int64_t peerId,
		std::int32_t msgId) {
	_lastError = code;
	_log.push_back({
		nowUnix,
		std::move(code),
		peerId,
		msgId,
		0,
	});
	while (int(_log.size()) > kLogCap) {
		_log.erase(_log.begin());
	}
}

int ChannelRules::forwardedOn(int day) const {
	return (_day == day) ? _dayCount : 0;
}

const std::string &ChannelRules::lastError() const {
	return _lastError;
}

const std::vector<ChannelLogRow> &ChannelRules::log() const {
	return _log;
}

std::string ChannelRules::dump() const {
	auto rules = nlohmann::json::array();
	for (const auto &rule : _rules) {
		rules.push_back({
			{ "id", rule.id },
			{ "source", rule.sourcePeerId },
			{ "target", rule.targetPeerId },
			{ "topic", rule.topicId },
			{ "enabled", rule.enabled },
			{ "enabledAt", rule.enabledAt },
			{ "paused", rule.paused },
			{ "pauseReason", rule.pauseReason },
		});
	}
	auto log = nlohmann::json::array();
	for (const auto &row : _log) {
		log.push_back({
			{ "ts", row.ts },
			{ "code", row.code },
			{ "peer", row.peerId },
			{ "msg", row.msgId },
			{ "target", row.targetPeerId },
		});
	}
	const auto json = nlohmann::json{
		{ "version", 1 },
		{ "accountId", _accountId },
		{ "global", _global },
		{ "nextId", _nextId },
		{ "rules", rules },
		{ "claims", _claims },
		{ "origins", _origins },
		{ "produced", _produced },
		{ "day", _day },
		{ "dayCount", _dayCount },
		{ "lastError", _lastError },
		{ "log", log },
	};
	return json.dump();
}

bool ChannelRules::load(
		std::string_view json,
		std::int64_t expectedAccount) {
	try {
		const auto parsed = nlohmann::json::parse(json);
		if (!parsed.is_object()
			|| parsed.value("accountId", std::int64_t(0)) != expectedAccount) {
			return false;
		}
		auto rules = std::vector<ChannelRule>();
		for (const auto &row : parsed.value("rules", nlohmann::json::array())) {
			auto rule = ChannelRule();
			rule.id = row.value("id", std::int64_t(0));
			rule.sourcePeerId = row.value("source", std::int64_t(0));
			rule.targetPeerId = row.value("target", std::int64_t(0));
			rule.topicId = row.value("topic", std::int32_t(0));
			rule.enabled = row.value("enabled", false);
			rule.enabledAt = row.value("enabledAt", std::int64_t(0));
			rule.paused = row.value("paused", false);
			rule.pauseReason = row.value("pauseReason", std::string());
			if (rule.sourcePeerId == rule.targetPeerId) {
				continue;
			}
			rules.push_back(std::move(rule));
		}
		auto log = std::vector<ChannelLogRow>();
		for (const auto &row : parsed.value("log", nlohmann::json::array())) {
			log.push_back({
				row.value("ts", std::int64_t(0)),
				row.value("code", std::string()),
				row.value("peer", std::int64_t(0)),
				row.value("msg", std::int32_t(0)),
				row.value("target", std::int64_t(0)),
			});
		}
		_global = parsed.value("global", false);
		_nextId = parsed.value("nextId", std::int64_t(1));
		_rules = std::move(rules);
		_claims = parsed.value("claims", std::vector<std::string>());
		_origins = parsed.value("origins", std::vector<std::string>());
		_produced = parsed.value("produced", std::vector<std::string>());
		_groups.clear();
		_day = parsed.value("day", 0);
		_dayCount = parsed.value("dayCount", 0);
		_lastError = parsed.value("lastError", std::string());
		_log = std::move(log);
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace Ayu::PrivateForward
