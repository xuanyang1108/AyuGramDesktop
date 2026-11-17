// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_channel_forward.h"

#include "ayu/features/private_forward/forward_controller.h"
#include "ayu/ui/settings/settings_main.h"
#include "base/call_delayed.h"
#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_channel.h"
#include "data/data_chat_participant_status.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_peer_id.h"
#include "history/history.h"
#include "lang_auto.h"
#include "rpl/event_stream.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_boxes.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

namespace Settings {
namespace {

using namespace Builder;
using Ayu::PrivateForward::Controller;

not_null<Controller*> Forwarder(
		not_null<Window::SessionController*> window) {
	return &Controller::Get(&window->session());
}

[[nodiscard]] PeerId PeerFromKey(std::int64_t key) {
	return PeerId(PeerIdHelper(std::uint64_t(key)));
}

[[nodiscard]] QString PeerLabel(
		not_null<Main::Session*> session,
		std::int64_t key) {
	if (!key) {
		return tr::ayu_ChannelForwardUnset(tr::now);
	}
	if (const auto peer = session->data().peerLoaded(PeerFromKey(key))) {
		const auto name = peer->name();
		return name.isEmpty() ? QString::number(key) : name;
	}
	return QString::number(key);
}

[[nodiscard]] QString RuleErrorText(Ayu::PrivateForward::RuleError error) {
	switch (error) {
	case Ayu::PrivateForward::RuleError::SamePeer:
		return tr::ayu_ChannelForwardSame(tr::now);
	case Ayu::PrivateForward::RuleError::SourceNotBroadcast:
		return tr::ayu_ChannelForwardBadSource(tr::now);
	case Ayu::PrivateForward::RuleError::TargetNotSendable:
		return tr::ayu_ChannelForwardBadTarget(tr::now);
	case Ayu::PrivateForward::RuleError::TopicRequired:
		return tr::ayu_ChannelForwardTopicRequired(tr::now);
	case Ayu::PrivateForward::RuleError::TopicUnexpected:
		return tr::ayu_ChannelForwardTopicUnexpected(tr::now);
	case Ayu::PrivateForward::RuleError::Duplicate:
		return tr::ayu_ChannelForwardDuplicate(tr::now);
	case Ayu::PrivateForward::RuleError::Missing:
		return tr::ayu_ChannelForwardMissing(tr::now);
	case Ayu::PrivateForward::RuleError::None:
		return QString();
	}
	return QString();
}

class BroadcastPick final : public ChatsListBoxController {
public:
	BroadcastPick(
		not_null<Main::Session*> session,
		Fn<void(not_null<PeerData*>)> chosen)
	: ChatsListBoxController(session)
	, _chosen(std::move(chosen)) {
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		_chosen(row->peer());
	}

protected:
	std::unique_ptr<Row> createRow(not_null<History*> history) override {
		const auto channel = history->peer->asChannel();
		if (!channel || !channel->isBroadcast() || !channel->amIn()) {
			return nullptr;
		}
		return std::make_unique<Row>(history);
	}
	void prepareViewHook() override {
		delegate()->peerListSetTitle(tr::ayu_ChannelForwardPickSource());
	}
	QString emptyBoxText() const override {
		return tr::ayu_ChannelForwardNoSource(tr::now);
	}

private:
	Fn<void(not_null<PeerData*>)> _chosen;

};

class TargetPick final : public ChatsListBoxController {
public:
	TargetPick(
		not_null<Main::Session*> session,
		std::int64_t sourceId,
		Fn<void(not_null<PeerData*>)> chosen)
	: ChatsListBoxController(session)
	, _sourceId(sourceId)
	, _chosen(std::move(chosen)) {
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		_chosen(row->peer());
	}

protected:
	std::unique_ptr<Row> createRow(not_null<History*> history) override {
		const auto peer = history->peer;
		if (std::int64_t(peer->id.value) == _sourceId) {
			return nullptr;
		}
		if (!Controller::CanSendTo(peer)) {
			return nullptr;
		}
		if (Controller::KindOfPeer(peer) == Ayu::PrivateForward::PeerKind::User
			|| Controller::KindOfPeer(peer) == Ayu::PrivateForward::PeerKind::Other) {
			return nullptr;
		}
		return std::make_unique<Row>(history);
	}
	void prepareViewHook() override {
		delegate()->peerListSetTitle(tr::ayu_ChannelForwardPickTarget());
	}
	QString emptyBoxText() const override {
		return tr::ayu_ChannelForwardNoTarget(tr::now);
	}

private:
	std::int64_t _sourceId = 0;
	Fn<void(not_null<PeerData*>)> _chosen;

};

void ShowPeerPicker(
		not_null<Window::SessionController*> window,
		std::unique_ptr<PeerListController> controller) {
	window->show(Box<PeerListBox>(
		std::move(controller),
		[=](not_null<PeerListBox*> box) {
			box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		}));
}

void EditRule(
		not_null<Window::SessionController*> window,
		not_null<Controller*> forwarder,
		Ayu::PrivateForward::ChannelRule existing) {
	const auto state = std::make_shared<Ayu::PrivateForward::ChannelRule>(
		existing);
	const auto sourceText = std::make_shared<rpl::event_stream<QString>>();
	const auto targetText = std::make_shared<rpl::event_stream<QString>>();
	const auto topicText = std::make_shared<rpl::event_stream<QString>>();
	const auto session = &window->session();
	const auto initialSource = PeerLabel(session, state->sourcePeerId);
	const auto initialTarget = PeerLabel(session, state->targetPeerId);
	const auto initialTopic = state->topicId
		? QString::number(state->topicId)
		: tr::ayu_ChannelForwardUnset(tr::now);
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(state->id
			? tr::ayu_ChannelForwardEdit()
			: tr::ayu_ChannelForwardAdd());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_ChannelForwardEditAbout(),
			st::boxLabel));
		box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			tr::ayu_ChannelForwardSource(),
			st::settingsButtonNoIcon
		))->addClickHandler([=] {
			ShowPeerPicker(
				window,
				std::make_unique<BroadcastPick>(
					session,
					[=](not_null<PeerData*> peer) {
						state->sourcePeerId = std::int64_t(peer->id.value);
						if (state->sourcePeerId == state->targetPeerId) {
							state->targetPeerId = 0;
							state->topicId = 0;
							targetText->fire(tr::ayu_ChannelForwardUnset(tr::now));
							topicText->fire(tr::ayu_ChannelForwardUnset(tr::now));
						}
						sourceText->fire(peer->name());
					}));
		});
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(initialSource) | rpl::then(sourceText->events()),
			st::boxLabel));
		box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			tr::ayu_ChannelForwardTarget(),
			st::settingsButtonNoIcon
		))->addClickHandler([=] {
			ShowPeerPicker(
				window,
				std::make_unique<TargetPick>(
					session,
					state->sourcePeerId,
					[=](not_null<PeerData*> peer) {
						state->targetPeerId = std::int64_t(peer->id.value);
						state->topicId = 0;
						targetText->fire(peer->name());
						topicText->fire(
							peer->isForum()
								? tr::ayu_ChannelForwardTopicRequired(tr::now)
								: tr::ayu_ChannelForwardNoTopicNeeded(tr::now));
						if (const auto forum = peer->forum()) {
							forum->requestTopics();
						}
					}));
		});
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(initialTarget) | rpl::then(targetText->events()),
			st::boxLabel));
		box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			tr::ayu_ChannelForwardTopic(),
			st::settingsButtonNoIcon
		))->addClickHandler([=] {
			const auto peer = session->data().peerLoaded(
				PeerFromKey(state->targetPeerId));
			if (!peer || !peer->isForum()) {
				window->showToast(tr::ayu_ChannelForwardNoTopicNeeded(tr::now));
				return;
			}
			const auto forum = peer->forum();
			if (!forum) {
				window->showToast(tr::ayu_ChannelForwardNoTopics(tr::now));
				return;
			}
			forum->requestTopics();
			auto topics = std::vector<std::pair<std::int32_t, QString>>();
			forum->enumerateTopics([&](not_null<Data::ForumTopic*> topic) {
				topics.emplace_back(
					std::int32_t(topic->rootId().bare),
					topic->title());
			});
			if (topics.empty()) {
				window->showToast(tr::ayu_ChannelForwardNoTopics(tr::now));
				return;
			}
			window->show(Box([=](not_null<Ui::GenericBox*> topicBox) {
				topicBox->setTitle(tr::ayu_ChannelForwardPickTopic());
				for (const auto &topic : topics) {
					const auto id = topic.first;
					const auto title = topic.second;
					topicBox->addRow(object_ptr<Ui::SettingsButton>(
						topicBox,
						rpl::single(title),
						st::settingsButtonNoIcon
					))->addClickHandler([=] {
						state->topicId = id;
						topicText->fire(title);
						topicBox->closeBox();
					});
				}
				topicBox->addButton(tr::lng_cancel(), [=] {
					topicBox->closeBox();
				});
			}));
		});
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(initialTopic) | rpl::then(topicText->events()),
			st::boxLabel));
		const auto save = [=] {
			const auto peer = session->data().peerLoaded(
				PeerFromKey(state->sourcePeerId));
			const auto target = session->data().peerLoaded(
				PeerFromKey(state->targetPeerId));
			if (!peer || !target) {
				window->showToast(tr::ayu_ChannelForwardMissing(tr::now));
				return;
			}
			auto rule = *state;
			const auto error = rule.id
				? forwarder->updateChannelRule(
					rule,
					Controller::KindOfPeer(peer),
					Controller::KindOfPeer(target),
					Controller::CanSendTo(target))
				: forwarder->addChannelRule(
					rule,
					Controller::KindOfPeer(peer),
					Controller::KindOfPeer(target),
					Controller::CanSendTo(target));
			if (error != Ayu::PrivateForward::RuleError::None) {
				window->showToast(RuleErrorText(error));
				return;
			}
			box->closeBox();
		};
		box->addButton(tr::lng_settings_save(), save);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		if (state->id) {
			box->addButton(tr::ayu_ChannelForwardDelete(), [=] {
				window->show(MakeConfirmBox({
					.text = tr::ayu_ChannelForwardDeleteConfirm(),
					.confirmed = [=](Fn<void()> close) {
						forwarder->removeChannelRule(state->id);
						close();
						box->closeBox();
					},
					.cancelled = [=](Fn<void()> close) { close(); },
				}));
			});
		}
	}));
}

class RuleList final : public Ui::RpWidget {
public:
	RuleList(
		QWidget *parent,
		not_null<Window::SessionController*> window,
		not_null<Controller*> forwarder)
	: RpWidget(parent)
	, _window(window)
	, _forwarder(forwarder) {
		forwarder->changes() | rpl::on_next([=] {
			if (_queued) {
				return;
			}
			_queued = true;
			base::call_delayed(crl::time(0), this, [=] {
				_queued = false;
				refill();
			});
		}, lifetime());
		refill();
	}

private:
	void refill() {
		delete _inner;
		_inner = Ui::CreateChild<Ui::VerticalLayout>(this);
		const auto session = &_window->session();
		const auto rules = _forwarder->channelRules();
		if (rules.empty()) {
			_inner->add(object_ptr<Ui::FlatLabel>(
				_inner,
				tr::ayu_ChannelForwardEmpty(),
				st::boxLabel));
		}
		for (const auto &rule : rules) {
			const auto id = rule.id;
			const auto title = PeerLabel(session, rule.sourcePeerId)
				+ u" → "_q
				+ PeerLabel(session, rule.targetPeerId)
				+ (rule.topicId
					? (u" #"_q + QString::number(rule.topicId))
					: QString());
			const auto button = _inner->add(object_ptr<Ui::SettingsButton>(
				_inner,
				rpl::single(title),
				st::settingsButtonNoIcon));
			button->toggleOn(rpl::single(rule.enabled && !rule.paused));
			button->setClickedCallback([=] {
				const auto current = _forwarder->channelRules();
				for (const auto &item : current) {
					if (item.id == id) {
						EditRule(_window, _forwarder, item);
						return;
					}
				}
			});
			button->toggledValue() | rpl::filter([=](bool enabled) {
				const auto current = _forwarder->channelRules();
				for (const auto &item : current) {
					if (item.id == id) {
						return enabled != item.enabled;
					}
				}
				return false;
			}) | rpl::on_next([=](bool enabled) {
				const auto error = _forwarder->setChannelRuleEnabled(id, enabled);
				if (error != Ayu::PrivateForward::RuleError::None) {
					_window->showToast(RuleErrorText(error));
					_forwarder->touch();
				}
			}, button->lifetime());
			if (rule.paused) {
				_inner->add(object_ptr<Ui::FlatLabel>(
					_inner,
					tr::ayu_ChannelForwardPaused(tr::now)
						+ u" "_q
						+ QString::fromStdString(rule.pauseReason),
					st::boxLabel));
			}
		}
		_inner->resizeToWidth(width());
		_inner->move(0, 0);
		setFixedHeight(_inner->height());
	}

	void resizeEvent(QResizeEvent *event) override {
		RpWidget::resizeEvent(event);
		if (_inner) {
			_inner->resizeToWidth(width());
			setFixedHeight(_inner->height());
		}
	}

	const not_null<Window::SessionController*> _window;
	const not_null<Controller*> _forwarder;
	Ui::VerticalLayout *_inner = nullptr;
	bool _queued = false;

};

void BuildPage(SectionBuilder &builder) {
	const auto window = builder.controller();
	const auto forwarder = Forwarder(window);
	builder.addSkip();
	const auto enabled = builder.addButton({
		.id = u"ayu/channelForward/global"_q,
		.title = tr::ayu_ChannelForwardEnable(),
		.st = &st::settingsButtonNoIcon,
		.icon = { &st::menuIconChannel },
	});
	if (enabled) {
		enabled->toggleOn(rpl::single(forwarder->channelGlobal()) | rpl::then(
			forwarder->changes() | rpl::map([=] {
				return forwarder->channelGlobal();
			})));
		enabled->toggledValue() | rpl::filter([=](bool value) {
			return value != forwarder->channelGlobal();
		}) | rpl::on_next([=](bool value) {
			if (!value) {
				forwarder->setChannelGlobal(false);
				return;
			}
			window->show(MakeConfirmBox({
				.text = tr::ayu_ChannelForwardEnableWarn(),
				.confirmed = [=](Fn<void()> close) {
					forwarder->setChannelGlobal(true);
					close();
				},
				.cancelled = [=](Fn<void()> close) {
					forwarder->touch();
					close();
				},
			}));
		}, enabled->lifetime());
	}
	builder.addDividerText(tr::ayu_ChannelForwardAbout());
	builder.addButton({
		.id = u"ayu/channelForward/add"_q,
		.title = tr::ayu_ChannelForwardAdd(),
		.st = &st::settingsButtonNoIcon,
		.onClick = [=] {
			EditRule(window, forwarder, {});
		},
	});
	builder.add([=](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		return {
			.widget = object_ptr<RuleList>(ctx.container, window, forwarder),
		};
	});
	builder.addDividerText(
		rpl::single(0) | rpl::then(
			forwarder->changes() | rpl::map([] { return 0; })
		) | rpl::map([=](int) {
			return tr::ayu_ChannelForwardToday(
					tr::now,
					lt_number,
					QString::number(forwarder->channelToday()))
				+ u"    "_q
				+ tr::ayu_PrivateForwardLastError(tr::now)
				+ u" "_q
				+ forwarder->channelLastError();
		}));
	builder.addButton({
		.id = u"ayu/channelForward/log"_q,
		.title = tr::ayu_PrivateForwardLog(),
		.st = &st::settingsButtonNoIcon,
		.onClick = [=] {
			window->show(Box([=](not_null<Ui::GenericBox*> box) {
				box->setTitle(tr::ayu_ChannelForwardLogTitle());
				const auto rows = forwarder->channelLogLines();
				if (rows.empty()) {
					box->addRow(object_ptr<Ui::FlatLabel>(
						box,
						tr::ayu_PrivateForwardLogEmpty(),
						st::boxLabel));
				}
				for (const auto &row : rows) {
					box->addRow(object_ptr<Ui::FlatLabel>(
						box,
						row,
						st::boxLabel));
				}
				box->addButton(tr::lng_close(), [=] { box->closeBox(); });
			}));
		},
	});
	builder.addSkip();
}

const auto kMeta = BuildHelper({
	.id = AyuChannelForward::Id(),
	.parentId = AyuMain::Id(),
	.title = &tr::ayu_ChannelForwardTitle,
	.icon = &st::menuIconChannel,
}, [](SectionBuilder &builder) {
	BuildPage(builder);
});

} // namespace

rpl::producer<QString> AyuChannelForward::title() {
	return tr::ayu_ChannelForwardTitle();
}

AyuChannelForward::AyuChannelForward(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void AyuChannelForward::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuChannelForwardId() {
	return AyuChannelForward::Id();
}

} // namespace Settings
