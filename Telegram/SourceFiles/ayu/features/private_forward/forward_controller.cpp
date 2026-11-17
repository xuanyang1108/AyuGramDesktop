// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/private_forward/forward_controller.h"

#include "api/api_chat_participants.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/flat_map.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "data/data_msg_id.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang_auto.h"
#include "rpl/event_stream.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

#include <deque>
#include <map>
#include <QDate>
#include <QFile>
#include <QtCore/QDateTime>

namespace Ayu::PrivateForward {
namespace {

constexpr auto kSendGap = crl::time(400);
constexpr auto kAlbumQuiet = crl::time(450);
constexpr auto kAdminCache = crl::time(15000);
constexpr auto kQueueCap = 400;
constexpr auto kAnonymousBotId = UserId(1087968824);
constexpr int kColors[] = {
	0x6FB9F0,
	0xFFD67E,
	0xCB86DB,
	0x8EEE98,
	0xFF93B2,
	0xFB6F5F,
};

enum Mark {
	MarkPending = 1,
	MarkSent = 2,
	MarkFailed = 3,
	MarkGroupOnly = 4,
};

[[nodiscard]] std::int64_t Key(not_null<PeerData*> peer) {
	return std::int64_t(peer->id.value);
}

[[nodiscard]] PeerId PeerFromKey(std::int64_t key) {
	return PeerId(PeerIdHelper(std::uint64_t(key)));
}

[[nodiscard]] QString MarkText() {
	return QString::fromUtf8(kLoopMarkUtf8);
}

[[nodiscard]] bool HasMark(const QString &text) {
	return text.startsWith(MarkText());
}

[[nodiscard]] QString WithMark(const QString &text) {
	return HasMark(text) ? text : (MarkText() + text);
}

[[nodiscard]] bool LooksLikeSecret(const QString &text) {
	const auto trimmed = text.trimmed();
	if (trimmed.size() >= 5 && trimmed.size() <= 8) {
		auto digits = true;
		for (const auto ch : trimmed) {
			if (!ch.isDigit()) {
				digits = false;
				break;
			}
		}
		if (digits) {
			return true;
		}
	}
	const auto lower = trimmed.toLower();
	return lower.contains(u"login code"_q)
		|| lower.contains(u"verification code"_q)
		|| lower.contains(u"验证码"_q);
}

[[nodiscard]] bool EligibleTarget(PeerData *peer) {
	const auto channel = peer ? peer->asChannel() : nullptr;
	if (!channel || !channel->isForum() || channel->isBroadcast()) {
		return false;
	}
	return channel->canCreateTopics()
		&& Data::CanSend(channel, ChatRestriction::SendOther, false);
}

[[nodiscard]] ContentKind KindOf(not_null<HistoryItem*> item) {
	const auto text = item->originalText().text;
	const auto media = item->media();
	if (!media || media->webpage()) {
		return text.isEmpty() ? ContentKind::None : ContentKind::Text;
	}
	if (media->photo()) {
		return ContentKind::Photo;
	}
	if (const auto document = media->document()) {
		if (document->forbidsFileSave()) {
			return ContentKind::Unsupported;
		}
		if (document->isVoiceMessage()) {
			return ContentKind::Voice;
		}
		if (document->isVideoMessage()
			|| document->isVideoFile()
			|| document->isAnimation()) {
			return ContentKind::Video;
		}
		if (document->type == StickerDocument) {
			return ContentKind::Sticker;
		}
		return ContentKind::File;
	}
	return ContentKind::Unsupported;
}

[[nodiscard]] bool IsProtected(not_null<HistoryItem*> item) {
	if (item->isEphemeral() || item->ttlDestroyAt() > 0) {
		return true;
	}
	const auto peer = item->history()->peer;
	if (peer->isAyuNoForwards()
		|| !peer->allowsForwarding()
		|| item->isAyuNoForwards()) {
		return true;
	}
	if (const auto media = item->media()) {
		if (!media->allowsForward()) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] int Today() {
	const auto date = QDate::currentDate();
	return date.year() * 10000 + date.month() * 100 + date.day();
}

[[nodiscard]] QString TopicTitle(
		not_null<PeerData*> peer,
		bool withUsername) {
	auto name = peer->name();
	name.replace('\n', ' ');
	name = name.trimmed();
	if (withUsername) {
		const auto username = peer->username();
		if (!username.isEmpty()) {
			name += u" (@"_q + username + u")"_q;
		}
	}
	if (name.isEmpty()) {
		name = u"私聊"_q;
	}
	return (name.size() > 128) ? name.left(128) : name;
}

[[nodiscard]] QString ContextLine(
		not_null<HistoryItem*> item,
		bool withUsername) {
	const auto peer = item->history()->peer;
	const auto own = item->out() || (item->from()->id == item->history()->session().userPeerId());
	auto name = own ? u"我"_q : TopicTitle(peer, withUsername);
	if (own) {
		name = u"我"_q;
	}
	const auto time = base::unixtime::parse(item->date()).time().toString(
		u"HH:mm"_q);
	return name + u' ' + time;
}

[[nodiscard]] QString StorePath(std::int64_t accountId) {
	return cWorkingDir()
		+ u"tdata/ayu_private_forward_"_q
		+ QString::number(accountId)
		+ u".json"_q;
}

struct SentIds {
	std::vector<std::int32_t> ids;
	std::int32_t topicId = 0;
};

void TakeMessage(const MTPMessage &message, SentIds &out) {
	if (message.type() != mtpc_message
		&& message.type() != mtpc_messageService) {
		return;
	}
	const auto id = std::int32_t(IdFromMessage(message).bare);
	if (id > 0
		&& std::find(out.ids.begin(), out.ids.end(), id) == out.ids.end()) {
		out.ids.push_back(id);
	}
	if (message.type() == mtpc_messageService) {
		const auto &data = message.c_messageService();
		if (data.vaction().type() == mtpc_messageActionTopicCreate) {
			out.topicId = data.vid().v;
		}
	}
}

void TakeUpdate(const MTPUpdate &update, SentIds &out) {
	update.match([&](const MTPDupdateNewMessage &data) {
		TakeMessage(data.vmessage(), out);
	}, [&](const MTPDupdateNewChannelMessage &data) {
		TakeMessage(data.vmessage(), out);
	}, [&](const MTPDupdateMessageID &data) {
		const auto id = std::int32_t(data.vid().v);
		if (id > 0
			&& std::find(out.ids.begin(), out.ids.end(), id) == out.ids.end()) {
			out.ids.push_back(id);
		}
	}, [](const auto &) {
	});
}

[[nodiscard]] SentIds ReadSent(const MTPUpdates &updates) {
	auto out = SentIds();
	updates.match([&](const MTPDupdates &data) {
		for (const auto &update : data.vupdates().v) {
			TakeUpdate(update, out);
		}
	}, [&](const MTPDupdatesCombined &data) {
		for (const auto &update : data.vupdates().v) {
			TakeUpdate(update, out);
		}
	}, [&](const MTPDupdateShort &data) {
		TakeUpdate(data.vupdate(), out);
	}, [](const auto &) {
	});
	return out;
}

[[nodiscard]] MTPInputReplyTo ForumReply(std::int32_t topicId) {
	using Flag = MTPDinputReplyToMessage::Flag;
	return MTP_inputReplyToMessage(
		MTP_flags(Flag::f_top_msg_id),
		MTP_int(topicId),
		MTP_int(topicId),
		MTPInputPeer(),
		MTP_string(),
		MTP_vector<MTPMessageEntity>(),
		MTP_int(0),
		MTPInputPeer(),
		MTP_int(0),
		MTP_bytes(QByteArray()));
}

[[nodiscard]] MTPInputReplyTo PrivateReply(std::int32_t msgId) {
	if (!msgId) {
		return MTPInputReplyTo();
	}
	return MTP_inputReplyToMessage(
		MTP_flags(0),
		MTP_int(msgId),
		MTP_int(0),
		MTPInputPeer(),
		MTP_string(),
		MTP_vector<MTPMessageEntity>(),
		MTP_int(0),
		MTPInputPeer(),
		MTP_int(0),
		MTP_bytes(QByteArray()));
}

[[nodiscard]] std::optional<MTPInputMedia> MediaInput(
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return std::nullopt;
	}
	if (const auto photo = media->photo()) {
		return MTP_inputMediaPhoto(
			MTP_flags(0),
			photo->mtpInput(),
			MTPint(),
			MTPInputDocument());
	}
	if (const auto document = media->document()) {
		return MTP_inputMediaDocument(
			MTP_flags(0),
			document->mtpInput(),
			MTPInputPhoto(),
			MTPint(),
			MTPint(),
			MTPstring());
	}
	return std::nullopt;
}

[[nodiscard]] int FloodSeconds(const QString &type) {
	const auto prefixes = {
		u"FLOOD_WAIT_"_q,
		u"FLOOD_PREMIUM_WAIT_"_q,
		u"SLOWMODE_WAIT_"_q,
	};
	for (const auto &prefix : prefixes) {
		if (type.startsWith(prefix)) {
			return std::max(1, type.mid(prefix.size()).toInt());
		}
	}
	return 0;
}

[[nodiscard]] bool TopicGone(const QString &type) {
	return type == u"TOPIC_DELETED"_q
		|| type == u"TOPIC_CLOSED"_q
		|| type == u"TOPIC_ID_INVALID"_q;
}

[[nodiscard]] bool PermissionError(const QString &type) {
	return type == u"CHAT_WRITE_FORBIDDEN"_q
		|| type == u"CHAT_ADMIN_REQUIRED"_q
		|| type == u"USER_BANNED_IN_CHANNEL"_q
		|| type == u"CHANNEL_PRIVATE"_q
		|| type == u"CHAT_RESTRICTED"_q;
}

auto &Controllers() {
	static auto map = base::flat_map<
		not_null<Main::Session*>,
		std::unique_ptr<Controller>>();
	return map;
}

} // namespace

struct Controller::Private {
	explicit Private(std::int64_t accountId)
	: engine(accountId) {
	}

	Engine engine;
	std::deque<Controller::Job> queue;
	bool busy = false;
	base::Timer sendTimer;
	base::Timer albumTimer;
	base::Timer saveTimer;
	rpl::event_stream<> updated;
	std::map<std::pair<std::uint64_t, std::int64_t>, int> marks;
	std::map<std::int64_t, std::pair<bool, crl::time>> admins;
	crl::time nextSend = 0;

};

Controller &Controller::Get(not_null<Main::Session*> session) {
	auto &slot = Controllers()[session];
	if (!slot) {
		slot = std::unique_ptr<Controller>(new Controller(session));
		session->lifetime().add([session] {
			Controllers().remove(session);
		});
	}
	return *slot;
}

Controller *Controller::TryGet(not_null<Main::Session*> session) {
	const auto it = Controllers().find(session);
	return (it == Controllers().end()) ? nullptr : it->second.get();
}

Controller::Controller(not_null<Main::Session*> session)
: _session(session)
, _private(std::make_unique<Private>(std::int64_t(session->userId().bare))) {
	QFile file(StorePath(_private->engine.accountId()));
	if (file.open(QIODevice::ReadOnly)) {
		_private->engine.load(
			file.readAll().toStdString(),
			_private->engine.accountId());
	}
	_private->sendTimer.setCallback([=] { pump(); });
	_private->albumTimer.setCallback([=] {
		const auto ready = _private->engine.takeReadyAlbums(
			crl::now(),
			kAlbumQuiet);
		for (const auto &album : ready) {
			enqueueAlbum(album.peerId, album.ids);
		}
	});
	_private->saveTimer.setCallback([=] { saveNow(); });
	_session->changes().realtimeNameUpdates(
	) | rpl::on_next([=](const Data::NameUpdate &update) {
		const auto peer = update.peer;
		if (!peer->isUser() || !_private->engine.running()) {
			return;
		}
		if (!_private->engine.topicFor(Key(peer))) {
			return;
		}
		auto job = Job();
		job.kind = JobKind::Rename;
		job.privatePeer = Key(peer);
		job.text = TopicTitle(
			peer,
			_private->engine.settings().titleWithUsername);
		for (auto &queued : _private->queue) {
			if (queued.kind == JobKind::Rename
				&& queued.privatePeer == job.privatePeer) {
				queued.text = job.text;
				return;
			}
		}
		if (int(_private->queue.size()) < kQueueCap) {
			_private->queue.push_back(std::move(job));
			pump();
		}
	}, _lifetime);
}

Controller::~Controller() {
	saveNow();
}

const Settings &Controller::settings() const {
	return _private->engine.settings();
}

bool Controller::running() const {
	return _private->engine.running();
}

QString Controller::statusLabel() const {
	const auto &settings = _private->engine.settings();
	if (!settings.enabled) {
		return tr::ayu_PrivateForwardOff(tr::now);
	}
	if (!settings.targetPeerId) {
		return tr::ayu_PrivateForwardNoTarget(tr::now);
	}
	if (!settings.perPeerTopic || _private->engine.paused()) {
		return tr::ayu_PrivateForwardPaused(tr::now);
	}
	if (const auto peer = _session->data().peerLoaded(
			PeerFromKey(settings.targetPeerId))) {
		if (!EligibleTarget(peer)) {
			return tr::ayu_PrivateForwardPaused(tr::now);
		}
	}
	return tr::ayu_PrivateForwardRunning(tr::now);
}

QString Controller::targetTitle() const {
	const auto id = _private->engine.settings().targetPeerId;
	if (!id) {
		return tr::ayu_PrivateForwardTargetEmpty(tr::now);
	}
	if (const auto peer = _session->data().peerLoaded(PeerFromKey(id))) {
		const auto name = peer->name();
		return name.isEmpty()
			? tr::ayu_PrivateForwardTargetEmpty(tr::now)
			: name;
	}
	return tr::ayu_PrivateForwardTargetEmpty(tr::now);
}

QString Controller::targetStatus() const {
	const auto id = _private->engine.settings().targetPeerId;
	if (!id) {
		return QString();
	}
	const auto peer = _session->data().peerLoaded(PeerFromKey(id));
	if (!peer) {
		return tr::ayu_PrivateForwardTargetBad(tr::now);
	}
	return EligibleTarget(peer)
		? tr::ayu_PrivateForwardTargetOk(tr::now)
		: tr::ayu_PrivateForwardTargetBad(tr::now);
}

int Controller::todayCount() const {
	return _private->engine.forwardedOn(Today());
}

QString Controller::lastErrorLabel() const {
	const auto code = QString::fromStdString(_private->engine.lastError());
	if (code.isEmpty()) {
		return tr::ayu_PrivateForwardNoError(tr::now);
	}
	if (FloodSeconds(code)) {
		return tr::ayu_PrivateForwardErrFlood(tr::now);
	}
	if (PermissionError(code)) {
		return tr::ayu_PrivateForwardErrPermission(tr::now);
	}
	if (TopicGone(code)) {
		return tr::ayu_PrivateForwardErrTopic(tr::now);
	}
	if (code == u"queue_overflow"_q) {
		return tr::ayu_PrivateForwardErrQueue(tr::now);
	}
	if (code == u"protected"_q) {
		return tr::ayu_PrivateForwardErrProtected(tr::now);
	}
	return code;
}

std::vector<QString> Controller::logLines() const {
	auto lines = std::vector<QString>();
	lines.reserve(_private->engine.log().size());
	for (const auto &row : _private->engine.log()) {
		const auto when = QDateTime::fromSecsSinceEpoch(row.ts).toString(
			u"yyyy-MM-dd HH:mm:ss"_q);
		lines.push_back(
			when
			+ u" "_q
			+ QString::fromStdString(row.code)
			+ u" peer="_q
			+ QString::number(row.peerId)
			+ u" msg="_q
			+ QString::number(row.msgId)
			+ u" topic="_q
			+ QString::number(row.topicId));
	}
	return lines;
}

rpl::producer<> Controller::changes() const {
	return _private->updated.events();
}

void Controller::touch() {
	_private->updated.fire({});
}

void Controller::remember(Fn<void(Settings &)> change) {
	auto settings = _private->engine.settings();
	change(settings);
	_private->engine.setSettings(std::move(settings));
	_private->engine.clearPaused();
	saveSoon();
	touch();
	pump();
}

void Controller::setEnabled(bool enabled) {
	remember([=](Settings &settings) { settings.enabled = enabled; });
	if (!enabled) {
		_private->queue.clear();
	}
}

void Controller::setPerPeerTopic(bool enabled) {
	remember([=](Settings &settings) { settings.perPeerTopic = enabled; });
}

void Controller::setTitleWithUsername(bool enabled) {
	remember([=](Settings &settings) {
		settings.titleWithUsername = enabled;
	});
}

void Controller::setAllowKind(ContentKind kind, bool enabled) {
	remember([=](Settings &settings) {
		switch (kind) {
		case ContentKind::Text: settings.allowText = enabled; break;
		case ContentKind::Photo: settings.allowPhoto = enabled; break;
		case ContentKind::Video: settings.allowVideo = enabled; break;
		case ContentKind::File: settings.allowFile = enabled; break;
		case ContentKind::Voice: settings.allowVoice = enabled; break;
		case ContentKind::Sticker: settings.allowSticker = enabled; break;
		case ContentKind::None:
		case ContentKind::Unsupported: break;
		}
	});
}

void Controller::setCopyWithoutHeader(bool enabled) {
	remember([=](Settings &settings) {
		settings.copyWithoutHeader = enabled;
	});
}

void Controller::setForwardOwn(bool enabled) {
	remember([=](Settings &settings) { settings.forwardOwn = enabled; });
}

void Controller::setEditDeleteHints(bool enabled) {
	remember([=](Settings &settings) {
		settings.editDeleteHints = enabled;
	});
}

void Controller::setRelayReplies(bool enabled) {
	remember([=](Settings &settings) {
		settings.relayReplies = enabled;
		if (enabled) {
			settings.relayEnabledAt = base::unixtime::now();
		}
	});
}

void Controller::setAllowOtherAdmins(bool enabled) {
	remember([=](Settings &settings) {
		settings.allowOtherAdmins = enabled && settings.relayReplies;
	});
}

void Controller::setIgnoreBots(bool enabled) {
	remember([=](Settings &settings) { settings.ignoreBots = enabled; });
}

void Controller::setIgnoreMuted(bool enabled) {
	remember([=](Settings &settings) { settings.ignoreMuted = enabled; });
}

bool Controller::setTarget(not_null<PeerData*> peer) {
	if (!EligibleTarget(peer)) {
		return false;
	}
	remember([=](Settings &settings) {
		settings.targetPeerId = Key(peer);
	});
	return true;
}

void Controller::toggleExcluded(std::int64_t peerId) {
	remember([=](Settings &settings) {
		const auto it = std::find(
			settings.excluded.begin(),
			settings.excluded.end(),
			peerId);
		if (it == settings.excluded.end()) {
			settings.excluded.push_back(peerId);
		} else {
			settings.excluded.erase(it);
		}
	});
}

void Controller::saveSoon() {
	_private->saveTimer.callOnce(crl::time(400));
}

void Controller::saveNow() {
	_private->saveTimer.cancel();
	QFile file(StorePath(_private->engine.accountId()));
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return;
	}
	const auto bytes = QByteArray::fromStdString(_private->engine.dump());
	if (file.write(bytes) != bytes.size()) {
		return;
	}
}

QString Controller::RelayMark(not_null<HistoryItem*> item) {
	const auto controller = TryGet(&item->history()->session());
	return controller ? controller->relayMark(item) : QString();
}

QString Controller::relayMark(not_null<HistoryItem*> item) const {
	const auto &settings = _private->engine.settings();
	if (!settings.relayReplies) {
		return {};
	}
	const auto peer = item->history()->peer;
	if (Key(peer) != settings.targetPeerId) {
		return {};
	}
	const auto topic = item->topicRootId();
	if (!topic
		|| !_private->engine.peerForTopic(std::int32_t(topic.bare))) {
		return {};
	}
	if (settings.relayEnabledAt
		&& item->date() < settings.relayEnabledAt) {
		return {};
	}
	const auto id = std::int32_t(item->id.bare);
	if (_private->engine.isProducedForum(id)) {
		return {};
	}
	const auto key = std::make_pair(peer->id.value, std::int64_t(item->id.bare));
	const auto found = _private->marks.find(key);
	if (found != _private->marks.end()) {
		switch (found->second) {
		case MarkPending: return tr::ayu_PrivateForwardPending(tr::now);
		case MarkSent: return tr::ayu_PrivateForwardSent(tr::now);
		case MarkFailed: return tr::ayu_PrivateForwardFailed(tr::now);
		case MarkGroupOnly: return tr::ayu_PrivateForwardGroupOnly(tr::now);
		}
	}
	if (_private->engine.wasRelayed(id)) {
		return tr::ayu_PrivateForwardSent(tr::now);
	}
	return {};
}

void Controller::setRelayMark(not_null<HistoryItem*> item, int mark) {
	const auto key = std::make_pair(
		item->history()->peer->id.value,
		std::int64_t(item->id.bare));
	_private->marks[key] = mark;
	item->history()->owner().requestItemResize(item);
}

void NotifyNewMessage(not_null<HistoryItem*> item) {
	Controller::Get(&item->history()->session()).handleNew(item);
}

void NotifyEdited(not_null<HistoryItem*> item) {
	Controller::Get(&item->history()->session()).handleEdited(item);
}

void NotifyDeleted(not_null<HistoryItem*> item) {
	Controller::Get(&item->history()->session()).handleDeleted(item);
}

void Controller::handleNew(not_null<HistoryItem*> item) {
	if (item->isScheduled() || item->isSending() || !IsServerMsgId(item->id)) {
		return;
	}
	const auto peer = item->history()->peer;
	const auto target = _private->engine.settings().targetPeerId;
	if (target && Key(peer) == target) {
		auto message = Snapshot();
		message.peerId = Key(peer);
		message.msgId = std::int32_t(item->id.bare);
		message.dateUnix = item->date();
		message.topicRootId = std::int32_t(item->topicRootId().bare);
		message.kind = KindOf(item);
		message.inTargetForum = true;
		message.serviceMessage = item->isService();
		message.forwarded = (item->Get<HistoryMessageForwarded>() != nullptr);
		message.loopMark = HasMark(item->originalText().text);
		const auto from = item->from();
		const auto user = from->asUser();
		message.authorIsSelf = (from->id == _session->userPeerId());
		message.authorIsBot = user && user->isBot();
		message.anonymousAdmin = !user
			|| (user->id == peerFromUser(kAnonymousBotId));
		const auto decision = _private->engine.decideRelay(message);
		if (decision.action == Action::Relay) {
			enqueueRelay(item, message.verifiedAdmin);
		} else if (decision.skip == Skip::NeedVerify) {
			verifyAdmin(item);
		} else if (decision.skip == Skip::GroupOnly) {
			setRelayMark(item, MarkGroupOnly);
		}
		return;
	}

	const auto user = peer->asUser();
	if (!user) {
		return;
	}
	auto message = Snapshot();
	message.peerId = Key(peer);
	message.msgId = std::int32_t(item->id.bare);
	message.groupedId = std::int64_t(item->groupId().raw());
	message.dateUnix = item->date();
	message.kind = KindOf(item);
	message.privateUser = true;
	message.savedMessages = user->isSelf();
	message.serviceChat = user->isSelf()
		|| peer->isServiceUser()
		|| peer->isNotificationsUser()
		|| peer->isVerifyCodes()
		|| peer->isRepliesChat();
	message.bot = user->isBot();
	message.outgoing = item->out();
	message.serviceMessage = item->isService();
	message.protectedContent = IsProtected(item);
	message.muted = _session->data().notifySettings().isMuted(peer);
	message.loopMark = HasMark(item->originalText().text);
	if ((message.serviceChat || message.bot)
		&& LooksLikeSecret(item->originalText().text)) {
		message.authSecret = true;
	}
	const auto decision = _private->engine.decideForward(message);
	if (decision.action != Action::Forward) {
		if (decision.skip == Skip::ProtectedContent) {
			_private->engine.noteError(
				base::unixtime::now(),
				"protected",
				message.peerId,
				message.msgId);
			saveSoon();
			touch();
		}
		return;
	}
	if (!_private->engine.claimForward(message.peerId, message.msgId)) {
		return;
	}
	if (message.groupedId) {
		_private->engine.addGrouped(
			message.peerId,
			message.groupedId,
			message.msgId,
			crl::now());
		_private->albumTimer.callOnce(kAlbumQuiet);
		return;
	}
	auto job = Job();
	job.kind = _private->engine.settings().copyWithoutHeader
		? JobKind::Copy
		: JobKind::Forward;
	job.privatePeer = message.peerId;
	job.sourcePeer = message.peerId;
	job.sourceIds = { message.msgId };
	job.randomIds = { base::RandomValue<std::uint64_t>() | 1 };
	if (int(_private->queue.size()) >= kQueueCap) {
		_private->engine.releaseForward(message.peerId, message.msgId);
		_private->engine.noteError(
			base::unixtime::now(),
			"queue_overflow",
			message.peerId,
			message.msgId);
		saveSoon();
		touch();
		return;
	}
	_private->queue.push_back(std::move(job));
	pump();
}

void Controller::enqueueAlbum(
		std::int64_t peerId,
		const std::vector<std::int32_t> &ids) {
	if (ids.empty() || !_private->engine.running()) {
		return;
	}
	auto job = Job();
	job.kind = _private->engine.settings().copyWithoutHeader
		? JobKind::Copy
		: JobKind::Forward;
	job.privatePeer = peerId;
	job.sourcePeer = peerId;
	job.sourceIds = ids;
	job.randomIds.reserve(ids.size());
	for (auto i = 0; i < int(ids.size()); ++i) {
		job.randomIds.push_back(base::RandomValue<std::uint64_t>() | 1);
	}
	if (int(_private->queue.size()) >= kQueueCap) {
		for (const auto id : ids) {
			_private->engine.releaseForward(peerId, id);
		}
		_private->engine.noteError(
			base::unixtime::now(),
			"queue_overflow",
			peerId,
			ids.front());
		saveSoon();
		return;
	}
	_private->queue.push_back(std::move(job));
	pump();
}

void Controller::enqueueRelay(
		not_null<HistoryItem*> item,
		bool verifiedAdmin) {
	const auto topic = std::int32_t(item->topicRootId().bare);
	const auto peerId = _private->engine.peerForTopic(topic);
	if (!peerId) {
		return;
	}
	auto job = Job();
	job.kind = JobKind::Relay;
	job.privatePeer = *peerId;
	job.sourcePeer = Key(item->history()->peer);
	job.topicId = topic;
	job.sourceIds = { std::int32_t(item->id.bare) };
	job.randomIds = { base::RandomValue<std::uint64_t>() | 1 };
	if (const auto replyId = item->replyToId()) {
		if (const auto original = _private->engine.privateForForum(
				std::int32_t(replyId.bare),
				*peerId)) {
			job.replyTo = *original;
		}
	}
	if (int(_private->queue.size()) >= kQueueCap) {
		setRelayMark(item, MarkFailed);
		_private->engine.noteError(
			base::unixtime::now(),
			"queue_overflow",
			*peerId,
			job.sourceIds.front());
		saveSoon();
		return;
	}
	setRelayMark(item, MarkPending);
	_private->engine.armEcho(*peerId, base::unixtime::now() + 120);
	_private->queue.push_back(job);
	Q_UNUSED(verifiedAdmin);
	pump();
}

void Controller::verifyAdmin(not_null<HistoryItem*> item) {
	const auto user = item->from()->asUser();
	const auto channel = item->history()->peer->asChannel();
	if (!user || !channel || user->isBot() || user->isSelf()) {
		setRelayMark(item, MarkGroupOnly);
		return;
	}
	const auto userKey = Key(user);
	const auto now = crl::now();
	if (const auto it = _private->admins.find(userKey);
		it != _private->admins.end() && it->second.second > now) {
		if (it->second.first) {
			enqueueRelay(item, true);
		} else {
			setRelayMark(item, MarkGroupOnly);
		}
		return;
	}
	const auto fullId = item->fullId();
	const auto session = _session;
	session->api().request(MTPchannels_GetParticipant(
		channel->inputChannel(),
		user->input()
	)).done(crl::guard(session, [=](
			const MTPchannels_ChannelParticipant &result) {
		const auto self = TryGet(session);
		if (!self) {
			return;
		}
		auto allowed = false;
		result.match([&](const MTPDchannels_channelParticipant &data) {
			channel->owner().processUsers(data.vusers());
			const auto parsed = Api::ChatParticipant(
				data.vparticipant(),
				channel);
			const auto loaded = channel->owner().userLoaded(parsed.userId());
			allowed = parsed.isCreatorOrAdmin()
				&& loaded
				&& !loaded->isBot()
				&& parsed.id() == user->id;
		});
		self->_private->admins[userKey] = { allowed, crl::now() + kAdminCache };
		const auto current = session->data().message(fullId);
		if (!current) {
			return;
		}
		if (allowed) {
			self->enqueueRelay(current, true);
		} else {
			self->setRelayMark(current, MarkGroupOnly);
			self->_private->engine.noteError(
				base::unixtime::now(),
				"admin_rejected",
				userKey,
				std::int32_t(current->id.bare));
			self->saveSoon();
		}
	})).fail(crl::guard(session, [=](const MTP::Error &) {
		const auto self = TryGet(session);
		if (!self) {
			return;
		}
		self->_private->admins[userKey] = { false, crl::now() + kAdminCache };
		if (const auto current = session->data().message(fullId)) {
			self->setRelayMark(current, MarkGroupOnly);
		}
	})).send();
}

void Controller::handleEdited(not_null<HistoryItem*> item) {
	const auto &settings = _private->engine.settings();
	if (!settings.enabled
		|| !settings.editDeleteHints
		|| !settings.perPeerTopic
		|| !_private->engine.running()) {
		return;
	}
	const auto peer = item->history()->peer->asUser();
	if (!peer || item->isService()) {
		return;
	}
	const auto edited = item->Get<HistoryMessageEdited>();
	if (!edited || edited->date + 600 < base::unixtime::now()) {
		return;
	}
	const auto peerId = Key(peer);
	const auto msgId = std::int32_t(item->id.bare);
	if (!_private->engine.forumForPrivate(peerId, msgId)) {
		return;
	}
	if (!_private->engine.claimEditHint(peerId, msgId, edited->date)) {
		return;
	}
	auto job = Job();
	job.kind = JobKind::Hint;
	job.privatePeer = peerId;
	job.sourcePeer = peerId;
	job.sourceIds = { msgId };
	job.text = WithMark(tr::ayu_PrivateForwardHintEdited(tr::now));
	job.randomIds = { base::RandomValue<std::uint64_t>() | 1 };
	if (int(_private->queue.size()) < kQueueCap) {
		_private->queue.push_back(std::move(job));
		pump();
	}
}

void Controller::handleDeleted(not_null<HistoryItem*> item) {
	const auto &settings = _private->engine.settings();
	if (!settings.enabled
		|| !settings.editDeleteHints
		|| !_private->engine.running()) {
		return;
	}
	const auto peer = item->history()->peer->asUser();
	if (!peer) {
		return;
	}
	const auto peerId = Key(peer);
	const auto msgId = std::int32_t(item->id.bare);
	if (!_private->engine.forumForPrivate(peerId, msgId)) {
		return;
	}
	if (!_private->engine.claimDeleteHint(peerId, msgId)) {
		return;
	}
	auto job = Job();
	job.kind = JobKind::Hint;
	job.privatePeer = peerId;
	job.sourcePeer = peerId;
	job.sourceIds = { msgId };
	job.text = WithMark(tr::ayu_PrivateForwardHintDeleted(tr::now));
	job.randomIds = { base::RandomValue<std::uint64_t>() | 1 };
	if (int(_private->queue.size()) < kQueueCap) {
		_private->queue.push_back(std::move(job));
		pump();
	}
}

void Controller::pump() {
	if (_private->busy || _private->queue.empty()) {
		return;
	}
	if (!_private->engine.running()) {
		return;
	}
	const auto nowUnix = base::unixtime::now();
	if (_private->engine.floodPaused(nowUnix)) {
		_private->sendTimer.callOnce(
			crl::time(_private->engine.floodWaitLeft(nowUnix) + 1) * 1000);
		return;
	}
	if (crl::now() < _private->nextSend) {
		_private->sendTimer.callOnce(_private->nextSend - crl::now());
		return;
	}
	auto &job = _private->queue.front();
	if (job.kind != JobKind::Relay) {
		if (const auto topic = _private->engine.topicFor(job.privatePeer)) {
			job.topicId = *topic;
		} else if (_private->engine.createPending(job.privatePeer)) {
			_private->sendTimer.callOnce(kSendGap);
			return;
		} else if (_private->engine.beginCreate(job.privatePeer)) {
			const auto peer = _session->data().peerLoaded(
				PeerFromKey(job.privatePeer));
			const auto target = _session->data().peerLoaded(
				PeerFromKey(_private->engine.settings().targetPeerId));
			const auto channel = target ? target->asChannel() : nullptr;
			if (!peer || !channel) {
				_private->engine.failCreate(job.privatePeer);
				_private->engine.noteError(
					nowUnix,
					"target_not_loaded",
					job.privatePeer,
					job.sourceIds.empty() ? 0 : job.sourceIds.front());
				_private->queue.pop_front();
				saveSoon();
				pump();
				return;
			}
			_private->busy = true;
			const auto color = kColors[
				std::size_t(std::uint64_t(job.privatePeer)) % 6];
			const auto title = TopicTitle(
				peer,
				_private->engine.settings().titleWithUsername);
			const auto session = _session;
			const auto privatePeer = job.privatePeer;
			session->api().request(MTPmessages_CreateForumTopic(
				MTP_flags(MTPmessages_CreateForumTopic::Flag::f_icon_color),
				channel->input(),
				MTP_string(title),
				MTP_int(color),
				MTPlong(),
				MTP_long(base::RandomValue<std::uint64_t>() | 1),
				MTPInputPeer()
			)).done(crl::guard(session, [=](const MTPUpdates &result) {
				const auto self = TryGet(session);
				if (!self) {
					return;
				}
				const auto sent = ReadSent(result);
				if (sent.topicId > 0) {
					self->_private->engine.finishCreate(
						privatePeer,
						sent.topicId);
					self->_private->engine.rememberProducedForum(
						sent.topicId);
				} else {
					self->_private->engine.failCreate(privatePeer);
					self->_private->engine.noteError(
						base::unixtime::now(),
						"topic_missing",
						privatePeer,
						0);
					if (!self->_private->queue.empty()) {
						self->_private->queue.pop_front();
					}
				}
				self->saveNow();
				session->api().applyUpdates(result);
				self->_private->busy = false;
				self->_private->nextSend = crl::now() + kSendGap;
				self->saveSoon();
				self->pump();
			})).fail(crl::guard(session, [=](const MTP::Error &error) {
				const auto self = TryGet(session);
				if (!self) {
					return;
				}
				self->_private->engine.failCreate(privatePeer);
				self->_private->busy = false;
				self->onSendError(error.type());
			})).send();
			return;
		}
	}
	sendFront();
}

void Controller::onSendError(const QString &type) {
	if (_private->queue.empty()) {
		pump();
		return;
	}
	auto &job = _private->queue.front();
	const auto msgId = job.sourceIds.empty() ? 0 : job.sourceIds.front();
	const auto nowUnix = base::unixtime::now();
	if (const auto seconds = FloodSeconds(type)) {
		_private->engine.noteFlood(seconds, nowUnix);
		_private->engine.noteError(
			nowUnix,
			type.toStdString(),
			job.privatePeer,
			msgId);
		saveSoon();
		touch();
		_private->sendTimer.callOnce(crl::time(seconds + 1) * 1000);
		return;
	}
	if (TopicGone(type) && !job.recreatedTopic && job.kind != JobKind::Relay) {
		_private->engine.clearTopic(job.privatePeer);
		job.topicId = 0;
		job.recreatedTopic = true;
		_private->engine.noteError(
			nowUnix,
			type.toStdString(),
			job.privatePeer,
			msgId);
		saveSoon();
		pump();
		return;
	}
	if (type == u"TOPIC_NOT_MODIFIED"_q) {
		_private->queue.pop_front();
		_private->nextSend = crl::now() + kSendGap;
		pump();
		return;
	}
	if (PermissionError(type)) {
		_private->engine.setPaused(type.toStdString());
		_private->engine.noteError(
			nowUnix,
			type.toStdString(),
			job.privatePeer,
			msgId);
		_private->queue.pop_front();
		saveSoon();
		touch();
		return;
	}
	if (++job.attempts < 3) {
		_private->sendTimer.callOnce(crl::time(1500));
		return;
	}
	_private->engine.noteError(
		nowUnix,
		type.toStdString(),
		job.privatePeer,
		msgId);
	if (job.kind == JobKind::Relay) {
		if (const auto item = _session->data().message(
				PeerFromKey(job.sourcePeer),
				MsgId(msgId))) {
			setRelayMark(item, MarkFailed);
			toast(tr::ayu_PrivateForwardFailed(tr::now) + u" "_q + type);
		}
	}
	_private->queue.pop_front();
	_private->nextSend = crl::now() + kSendGap;
	saveSoon();
	touch();
	pump();
}

void Controller::toast(const QString &text) {
	if (const auto window = _session->tryResolveWindow()) {
		window->showToast(text);
	}
}

void Controller::sendFront() {
	if (_private->queue.empty()) {
		return;
	}
	auto &job = _private->queue.front();
	const auto target = _session->data().peerLoaded(
		PeerFromKey(_private->engine.settings().targetPeerId));
	if (job.kind != JobKind::Relay && (!target || !job.topicId)) {
		onSendError(u"target_not_loaded"_q);
		return;
	}
	_private->busy = true;
	const auto session = _session;
	if (job.kind == JobKind::Rename) {
		session->api().request(MTPmessages_EditForumTopic(
			MTP_flags(MTPmessages_EditForumTopic::Flag::f_title),
			target->input(),
			MTP_int(job.topicId),
			MTP_string(job.text),
			MTPlong(),
			MTPBool(),
			MTPBool()
		)).done(crl::guard(session, [=](const MTPUpdates &result) {
			const auto self = TryGet(session);
			if (!self) {
				return;
			}
			session->api().applyUpdates(result);
			self->dropFront();
		})).fail(crl::guard(session, [=](const MTP::Error &error) {
			const auto self = TryGet(session);
			if (!self) {
				return;
			}
			self->_private->busy = false;
			self->onSendError(error.type());
		})).send();
		return;
	}
	if (job.kind == JobKind::Hint) {
		sendText(target, job.topicId, job.text, job.randomIds.front(), 0, true);
		return;
	}
	if (job.kind == JobKind::Forward) {
		sendForward(target, job);
		return;
	}
	sendCopy(job.kind == JobKind::Relay ? nullptr : target.get(), job);
}

void Controller::dropFront() {
	if (!_private->queue.empty()) {
		_private->queue.pop_front();
	}
	_private->busy = false;
	_private->nextSend = crl::now() + kSendGap;
	saveSoon();
	pump();
}

void Controller::acceptResult(const MTPUpdates &result, bool forumSide) {
	if (_private->queue.empty()) {
		_private->busy = false;
		return;
	}
	auto job = _private->queue.front();
	const auto sent = ReadSent(result);
	if (forumSide) {
		for (const auto id : sent.ids) {
			_private->engine.rememberProducedForum(id);
		}
	} else {
		for (const auto id : sent.ids) {
			_private->engine.rememberProducedPrivate(job.privatePeer, id);
		}
	}
	saveNow();
	_session->api().applyUpdates(result);
	if (forumSide && sent.ids.size() == job.sourceIds.size()) {
		for (auto i = 0; i < int(sent.ids.size()); ++i) {
			_private->engine.linkMessages(
				job.privatePeer,
				job.sourceIds[i],
				sent.ids[i]);
		}
	}
	if ((job.kind == JobKind::Forward || job.kind == JobKind::Copy)
		&& !sent.ids.empty()) {
		_private->engine.noteForwardSuccess(
			Today(),
			base::unixtime::now(),
			job.privatePeer,
			job.sourceIds.empty() ? 0 : job.sourceIds.front(),
			job.topicId,
			int(job.sourceIds.size()));
	}
	if (job.kind == JobKind::Relay) {
		for (const auto id : job.sourceIds) {
			_private->engine.rememberRelay(id);
			if (const auto item = _session->data().message(
					PeerFromKey(job.sourcePeer),
					MsgId(id))) {
				setRelayMark(item, MarkSent);
			}
		}
		toast(tr::ayu_PrivateForwardSent(tr::now));
	}
	dropFront();
	touch();
}

void Controller::sendText(
		PeerData *peer,
		std::int32_t topicId,
		const QString &text,
		std::uint64_t randomId,
		std::int32_t replyTo,
		bool inForum) {
	using Flag = MTPmessages_SendMessage::Flag;
	auto flags = Flag(0);
	auto reply = MTPInputReplyTo();
	if (inForum && topicId) {
		flags |= Flag::f_reply_to;
		reply = ForumReply(topicId);
	} else if (replyTo) {
		flags |= Flag::f_reply_to;
		reply = PrivateReply(replyTo);
	}
	const auto session = _session;
	session->api().request(MTPmessages_SendMessage(
		MTP_flags(flags),
		peer->input(),
		reply,
		MTP_string(text),
		MTP_long(randomId),
		MTPReplyMarkup(),
		MTPVector<MTPMessageEntity>(),
		MTPint(),
		MTPint(),
		MTP_inputPeerEmpty(),
		MTPInputQuickReplyShortcut(),
		MTP_long(0),
		MTP_long(0),
		Api::SuggestToMTP({}),
		MTPInputRichMessage()
	)).done(crl::guard(session, [=](const MTPUpdates &result) {
		const auto self = TryGet(session);
		if (!self) {
			return;
		}
		self->acceptResult(result, inForum);
	})).fail(crl::guard(session, [=](const MTP::Error &error) {
		const auto self = TryGet(session);
		if (!self) {
			return;
		}
		self->_private->busy = false;
		self->onSendError(error.type());
	})).send();
}

void Controller::sendForward(PeerData *target, const Job &job) {
	const auto from = _session->data().peerLoaded(PeerFromKey(job.sourcePeer));
	if (!from || job.sourceIds.empty()) {
		_private->engine.noteError(
			base::unixtime::now(),
			"message_gone",
			job.privatePeer,
			job.sourceIds.empty() ? 0 : job.sourceIds.front());
		dropFront();
		return;
	}
	auto ids = QVector<MTPint>();
	auto randomIds = QVector<MTPlong>();
	for (const auto id : job.sourceIds) {
		ids.push_back(MTP_int(id));
	}
	for (const auto randomId : job.randomIds) {
		randomIds.push_back(MTP_long(randomId));
	}
	using Flag = MTPmessages_ForwardMessages::Flag;
	const auto session = _session;
	session->api().request(MTPmessages_ForwardMessages(
		MTP_flags(Flag::f_top_msg_id),
		from->input(),
		MTP_vector<MTPint>(ids),
		MTP_vector<MTPlong>(randomIds),
		target->input(),
		MTP_int(job.topicId),
		MTPInputReplyTo(),
		MTPint(),
		MTPint(),
		MTP_inputPeerEmpty(),
		MTPInputQuickReplyShortcut(),
		MTP_long(0),
		MTPint(),
		MTP_long(0),
		Api::SuggestToMTP({})
	)).done(crl::guard(session, [=](const MTPUpdates &result) {
		const auto self = TryGet(session);
		if (!self) {
			return;
		}
		self->acceptResult(result, true);
	})).fail(crl::guard(session, [=](const MTP::Error &error) {
		const auto self = TryGet(session);
		if (!self) {
			return;
		}
		self->_private->busy = false;
		self->onSendError(error.type());
	})).send();
}

void Controller::sendCopy(PeerData *forum, const Job &job) {
	const auto destination = forum
		? forum
		: _session->data().peerLoaded(PeerFromKey(job.privatePeer)).get();
	if (!destination) {
		_private->busy = false;
		onSendError(u"target_not_loaded"_q);
		return;
	}
	auto items = std::vector<not_null<HistoryItem*>>();
	for (const auto id : job.sourceIds) {
		if (const auto item = _session->data().message(
				PeerFromKey(job.sourcePeer),
				MsgId(id))) {
			items.push_back(item);
		}
	}
	if (items.empty()) {
		_private->engine.noteError(
			base::unixtime::now(),
			"message_gone",
			job.privatePeer,
			job.sourceIds.empty() ? 0 : job.sourceIds.front());
		dropFront();
		return;
	}
	const auto inForum = (forum != nullptr);
	const auto withUsername = _private->engine.settings().titleWithUsername;
	if (items.size() == 1 && !MediaInput(items.front())) {
		const auto item = items.front();
		auto text = item->originalText().text;
		if (inForum) {
			text = ContextLine(item, withUsername) + u"\n"_q + text;
		}
		sendText(
			destination,
			job.topicId,
			WithMark(text),
			job.randomIds.front(),
			inForum ? 0 : job.replyTo,
			inForum);
		return;
	}
	if (items.size() == 1) {
		const auto item = items.front();
		auto caption = item->originalText().text;
		if (inForum) {
			caption = ContextLine(item, withUsername)
				+ (caption.isEmpty() ? QString() : (u"\n"_q + caption));
		}
		caption = WithMark(caption);
		const auto media = MediaInput(item);
		if (!media) {
			sendText(
				destination,
				job.topicId,
				caption,
				job.randomIds.front(),
				inForum ? 0 : job.replyTo,
				inForum);
			return;
		}
		using Flag = MTPmessages_SendMedia::Flag;
		auto flags = Flag(0);
		auto reply = MTPInputReplyTo();
		if (inForum) {
			flags |= Flag::f_reply_to;
			reply = ForumReply(job.topicId);
		} else if (job.replyTo) {
			flags |= Flag::f_reply_to;
			reply = PrivateReply(job.replyTo);
		}
		const auto session = _session;
		const auto randomId = job.randomIds.front();
		session->api().request(MTPmessages_SendMedia(
			MTP_flags(flags),
			destination->input(),
			reply,
			*media,
			MTP_string(caption),
			MTP_long(randomId),
			MTPReplyMarkup(),
			MTP_vector<MTPMessageEntity>(),
			MTPint(),
			MTPint(),
			MTPInputPeer(),
			MTPInputQuickReplyShortcut(),
			MTPlong(),
			MTP_long(0),
			Api::SuggestToMTP({})
		)).done(crl::guard(session, [=](const MTPUpdates &result) {
			const auto self = TryGet(session);
			if (!self) {
				return;
			}
			self->acceptResult(result, inForum);
		})).fail(crl::guard(session, [=](const MTP::Error &error) {
			const auto self = TryGet(session);
			if (!self) {
				return;
			}
			self->_private->busy = false;
			self->onSendError(error.type());
		})).send();
		return;
	}
	auto media = QVector<MTPInputSingleMedia>();
	auto index = 0;
	for (const auto &item : items) {
		const auto input = MediaInput(item);
		if (!input) {
			++index;
			continue;
		}
		auto caption = item->originalText().text;
		if (inForum && media.isEmpty()) {
			caption = ContextLine(item, withUsername)
				+ (caption.isEmpty() ? QString() : (u"\n"_q + caption));
		}
		const auto randomId = (index < int(job.randomIds.size()))
			? job.randomIds[index]
			: (base::RandomValue<std::uint64_t>() | 1);
		media.push_back(MTP_inputSingleMedia(
			MTP_flags(0),
			*input,
			MTP_long(randomId),
			MTP_string(WithMark(caption)),
			MTP_vector<MTPMessageEntity>()));
		++index;
	}
	if (media.isEmpty()) {
		_private->engine.noteError(
			base::unixtime::now(),
			"unsupported",
			job.privatePeer,
			job.sourceIds.front());
		dropFront();
		return;
	}
	using Flag = MTPmessages_SendMultiMedia::Flag;
	auto flags = Flag(0);
	auto reply = MTPInputReplyTo();
	if (inForum) {
		flags |= Flag::f_reply_to;
		reply = ForumReply(job.topicId);
	} else if (job.replyTo) {
		flags |= Flag::f_reply_to;
		reply = PrivateReply(job.replyTo);
	}
	const auto session = _session;
	session->api().request(MTPmessages_SendMultiMedia(
		MTP_flags(flags),
		destination->input(),
		reply,
		MTP_vector<MTPInputSingleMedia>(media),
		MTPint(),
		MTP_inputPeerEmpty(),
		MTPInputQuickReplyShortcut(),
		MTP_long(0),
		MTP_long(0)
	)).done(crl::guard(session, [=](const MTPUpdates &result) {
		const auto self = TryGet(session);
		if (!self) {
			return;
		}
		self->acceptResult(result, inForum);
	})).fail(crl::guard(session, [=](const MTP::Error &error) {
		const auto self = TryGet(session);
		if (!self) {
			return;
		}
		self->_private->busy = false;
		self->onSendError(error.type());
	})).send();
}

} // namespace Ayu::PrivateForward
