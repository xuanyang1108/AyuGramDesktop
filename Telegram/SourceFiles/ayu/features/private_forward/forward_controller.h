// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ayu/features/private_forward/forward_policy.h"
#include "apiwrap.h"
#include "main/main_session.h"
#include "rpl/lifetime.h"
#include "rpl/producer.h"

#include <memory>
#include <vector>

class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Ayu::PrivateForward {

class Controller {
public:
	static Controller &Get(not_null<Main::Session*> session);
	[[nodiscard]] static Controller *TryGet(not_null<Main::Session*> session);
	~Controller();

	[[nodiscard]] const Settings &settings() const;
	[[nodiscard]] bool running() const;
	[[nodiscard]] QString statusLabel() const;
	[[nodiscard]] QString targetTitle() const;
	[[nodiscard]] QString targetStatus() const;
	[[nodiscard]] int todayCount() const;
	[[nodiscard]] QString lastErrorLabel() const;
	[[nodiscard]] std::vector<QString> logLines() const;
	[[nodiscard]] rpl::producer<> changes() const;

	void setEnabled(bool enabled);
	void setPerPeerTopic(bool enabled);
	void setTitleWithUsername(bool enabled);
	void setAllowKind(ContentKind kind, bool enabled);
	void setCopyWithoutHeader(bool enabled);
	void setForwardOwn(bool enabled);
	void setEditDeleteHints(bool enabled);
	void setRelayReplies(bool enabled);
	void setAllowOtherAdmins(bool enabled);
	void setIgnoreBots(bool enabled);
	void setIgnoreMuted(bool enabled);
	[[nodiscard]] bool setTarget(not_null<PeerData*> peer);
	void toggleExcluded(std::int64_t peerId);
	void touch();

	[[nodiscard]] static QString RelayMark(not_null<HistoryItem*> item);

private:
	explicit Controller(not_null<Main::Session*> session);

	enum class JobKind {
		Forward,
		Copy,
		Relay,
		Hint,
		Rename,
	};
	struct Job {
		JobKind kind = JobKind::Forward;
		std::int64_t privatePeer = 0;
		std::int64_t sourcePeer = 0;
		std::int32_t topicId = 0;
		std::vector<std::int32_t> sourceIds;
		QString text;
		std::vector<std::uint64_t> randomIds;
		std::int32_t replyTo = 0;
		int attempts = 0;
		bool recreatedTopic = false;
	};

	struct Private;
	void remember(Fn<void(Settings &)> change);
	void saveSoon();
	void saveNow();
	void pump();
	void enqueueAlbum(
		std::int64_t peerId,
		const std::vector<std::int32_t> &ids);
	void enqueueRelay(not_null<HistoryItem*> item, bool verifiedAdmin);
	void verifyAdmin(not_null<HistoryItem*> item);
	void handleNew(not_null<HistoryItem*> item);
	void handleEdited(not_null<HistoryItem*> item);
	void handleDeleted(not_null<HistoryItem*> item);
	void setRelayMark(not_null<HistoryItem*> item, int mark);
	[[nodiscard]] QString relayMark(not_null<HistoryItem*> item) const;
	void onSendError(const QString &type);
	void toast(const QString &text);
	void sendFront();
	void dropFront();
	void acceptResult(const MTPUpdates &result, bool forumSide);
	void sendText(
		PeerData *peer,
		std::int32_t topicId,
		const QString &text,
		std::uint64_t randomId,
		std::int32_t replyTo,
		bool inForum);
	void sendForward(PeerData *target, const Job &job);
	void sendCopy(PeerData *forum, const Job &job);

	const not_null<Main::Session*> _session;
	std::unique_ptr<Private> _private;
	rpl::lifetime _lifetime;

	friend void NotifyNewMessage(not_null<HistoryItem*> item);
	friend void NotifyEdited(not_null<HistoryItem*> item);
	friend void NotifyDeleted(not_null<HistoryItem*> item);

};

void NotifyNewMessage(not_null<HistoryItem*> item);
void NotifyEdited(not_null<HistoryItem*> item);
void NotifyDeleted(not_null<HistoryItem*> item);

} // namespace Ayu::PrivateForward
