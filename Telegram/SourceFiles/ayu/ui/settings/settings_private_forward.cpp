// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_private_forward.h"

#include "ayu/ui/settings/settings_main.h"

#include "ayu/features/private_forward/forward_controller.h"
#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_channel.h"
#include "data/data_chat_participant_status.h"
#include "history/history.h"
#include "lang_auto.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <algorithm>
#include <array>

namespace Settings {
namespace {

using namespace Builder;

not_null<Ayu::PrivateForward::Controller*> Forwarder(
		not_null<Window::SessionController*> controller) {
	return &Ayu::PrivateForward::Controller::Get(&controller->session());
}

[[nodiscard]] rpl::producer<QString> LiveText(
		not_null<Ayu::PrivateForward::Controller*> forwarder,
		Fn<QString()> text) {
	return rpl::single(0) | rpl::then(
		forwarder->changes() | rpl::map([] { return 0; })
	) | rpl::map([=](int) {
		return text();
	});
}

void BindToggle(
		Ui::SettingsButton *button,
		not_null<Ayu::PrivateForward::Controller*> forwarder,
		Fn<bool()> get,
		Fn<void(bool)> set) {
	if (!button) {
		return;
	}
	button->toggleOn(rpl::single(get()) | rpl::then(
		forwarder->changes() | rpl::map([=] { return get(); })));
	button->toggledValue(
	) | rpl::filter([=](bool value) {
		return value != get();
	}) | rpl::on_next(std::move(set), button->lifetime());
}

void ConfirmEnable(
		not_null<Window::SessionController*> window,
		not_null<Ayu::PrivateForward::Controller*> forwarder,
		rpl::producer<QString> text,
		Fn<void()> apply) {
	window->show(MakeConfirmBox({
		.text = std::move(text),
		.confirmed = [=](Fn<void()> close) {
			apply();
			close();
		},
		.cancelled = [=](Fn<void()> close) {
			forwarder->touch();
			close();
		},
	}));
}

class ForumPickController final : public ChatsListBoxController {
public:
	ForumPickController(
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
		const auto peer = history->peer;
		const auto channel = peer->asChannel();
		if (!channel
			|| !channel->isForum()
			|| channel->isBroadcast()
			|| !channel->canCreateTopics()
			|| !Data::CanSend(channel, ChatRestriction::SendOther, false)) {
			return nullptr;
		}
		return std::make_unique<Row>(history);
	}
	void prepareViewHook() override {
		delegate()->peerListSetTitle(tr::ayu_PrivateForwardPick());
	}
	QString emptyBoxText() const override {
		return tr::ayu_PrivateForwardNoForum(tr::now);
	}

private:
	Fn<void(not_null<PeerData*>)> _chosen;

};

class ExcludeController final : public ChatsListBoxController {
public:
	ExcludeController(
		not_null<Main::Session*> session,
		not_null<Ayu::PrivateForward::Controller*> forwarder)
	: ChatsListBoxController(session)
	, _forwarder(forwarder) {
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		_forwarder->toggleExcluded(std::int64_t(row->peer()->id.value));
		refreshStatus(row);
		delegate()->peerListUpdateRow(row);
	}

protected:
	std::unique_ptr<Row> createRow(not_null<History*> history) override {
		const auto user = history->peer->asUser();
		if (!user
			|| user->isSelf()
			|| history->peer->isServiceUser()
			|| history->peer->isNotificationsUser()
			|| history->peer->isVerifyCodes()
			|| history->peer->isRepliesChat()) {
			return nullptr;
		}
		auto row = std::make_unique<Row>(history);
		refreshStatus(row.get());
		return row;
	}
	void prepareViewHook() override {
		delegate()->peerListSetTitle(tr::ayu_PrivateForwardExcludeList());
	}

private:
	void refreshStatus(not_null<PeerListRow*> row) const {
		const auto id = std::int64_t(row->peer()->id.value);
		const auto &excluded = _forwarder->settings().excluded;
		const auto on = std::find(excluded.begin(), excluded.end(), id)
			!= excluded.end();
		row->setCustomStatus(on ? u"已排除"_q : QString());
	}

	const not_null<Ayu::PrivateForward::Controller*> _forwarder;

};

class ContentRow final : public Ui::RpWidget {
public:
	ContentRow(
		QWidget *parent,
		not_null<Ayu::PrivateForward::Controller*> forwarder)
	: RpWidget(parent)
	, _forwarder(forwarder) {
		const auto kinds = std::array<Ayu::PrivateForward::ContentKind, 6>{
			Ayu::PrivateForward::ContentKind::Text,
			Ayu::PrivateForward::ContentKind::Photo,
			Ayu::PrivateForward::ContentKind::Video,
			Ayu::PrivateForward::ContentKind::File,
			Ayu::PrivateForward::ContentKind::Voice,
			Ayu::PrivateForward::ContentKind::Sticker,
		};
		const auto titles = std::array<QString, 6>{
			tr::ayu_PrivateForwardText(tr::now),
			tr::ayu_PrivateForwardPhoto(tr::now),
			tr::ayu_PrivateForwardVideo(tr::now),
			tr::ayu_PrivateForwardFile(tr::now),
			tr::ayu_PrivateForwardVoice(tr::now),
			tr::ayu_PrivateForwardSticker(tr::now),
		};
		for (auto i = 0; i < 6; ++i) {
			const auto kind = kinds[i];
			const auto checkbox = CreateChild<Ui::Checkbox>(
				this,
				titles[i],
				Allowed(forwarder->settings(), kind),
				st::settingsCheckbox);
			checkbox->resizeToWidth(checkbox->sizeHint().width());
			checkbox->checkedChanges(
			) | rpl::filter([=](bool checked) {
				return checked != Allowed(_forwarder->settings(), kind);
			}) | rpl::on_next([=](bool checked) {
				_forwarder->setAllowKind(kind, checked);
			}, lifetime());
			_boxes[i] = checkbox;
		}
		forwarder->changes(
		) | rpl::on_next([=] {
			for (auto i = 0; i < 6; ++i) {
				const auto want = Allowed(_forwarder->settings(), kinds[i]);
				if (_boxes[i]->checked() != want) {
					_boxes[i]->setChecked(want, anim::type::instant);
				}
			}
		}, lifetime());
		sizeValue(
		) | rpl::on_next([=](QSize size) {
			layout(size.width());
		}, lifetime());
	}

private:
	static bool Allowed(
			const Ayu::PrivateForward::Settings &settings,
			Ayu::PrivateForward::ContentKind kind) {
		switch (kind) {
		case Ayu::PrivateForward::ContentKind::Text:
			return settings.allowText;
		case Ayu::PrivateForward::ContentKind::Photo:
			return settings.allowPhoto;
		case Ayu::PrivateForward::ContentKind::Video:
			return settings.allowVideo;
		case Ayu::PrivateForward::ContentKind::File:
			return settings.allowFile;
		case Ayu::PrivateForward::ContentKind::Voice:
			return settings.allowVoice;
		case Ayu::PrivateForward::ContentKind::Sticker:
			return settings.allowSticker;
		case Ayu::PrivateForward::ContentKind::None:
		case Ayu::PrivateForward::ContentKind::Unsupported:
			return false;
		}
		return false;
	}

	void layout(int width) {
		auto x = 0;
		auto y = 0;
		auto rowHeight = 0;
		for (const auto checkbox : _boxes) {
			const auto boxWidth = checkbox->width();
			const auto boxHeight = checkbox->height();
			if (x > 0 && x + boxWidth > width) {
				x = 0;
				y += rowHeight + st::lineWidth;
				rowHeight = 0;
			}
			checkbox->move(x, y);
			x += boxWidth + st::defaultVerticalListSkip;
			rowHeight = std::max(rowHeight, boxHeight);
		}
		const auto height = y + rowHeight;
		if (this->height() != height) {
			resize(width, height);
		}
	}

	const not_null<Ayu::PrivateForward::Controller*> _forwarder;
	std::array<Ui::Checkbox*, 6> _boxes = {};

};

void BuildPage(SectionBuilder &builder) {
	const auto window = builder.controller();
	const auto forwarder = Forwarder(window);

	builder.addSkip();
	builder.addDividerText(LiveText(forwarder, [=] {
		return forwarder->statusLabel();
	}));
	builder.addDividerText(tr::ayu_PrivateForwardLimitations());

	const auto enabled = builder.addButton({
		.id = u"ayu/privateForward/enabled"_q,
		.title = tr::ayu_PrivateForwardEnable(),
		.st = &st::settingsButtonNoIcon,
		.icon = { &st::menuIconForward },
	});
	BindToggle(enabled, forwarder,
		[=] { return forwarder->settings().enabled; },
		[=](bool value) {
			if (!value) {
				forwarder->setEnabled(false);
				return;
			}
			ConfirmEnable(
				window,
				forwarder,
				tr::ayu_PrivateForwardEnableWarn(),
				[=] { forwarder->setEnabled(true); });
		});
	builder.addDividerText(tr::ayu_PrivateForwardEnableAbout());

	builder.addSubsectionTitle(tr::ayu_PrivateForwardTarget());
	builder.addButton({
		.id = u"ayu/privateForward/target"_q,
		.title = LiveText(forwarder, [=] {
			return forwarder->targetTitle();
		}),
		.st = &st::settingsButtonNoIcon,
		.label = LiveText(forwarder, [=] {
			const auto status = forwarder->targetStatus();
			return status.isEmpty()
				? tr::ayu_PrivateForwardPick(tr::now)
				: status;
		}),
		.onClick = [=] {
			const auto weak = std::make_shared<
				base::weak_qptr<PeerListBox>>();
			auto picker = std::make_unique<ForumPickController>(
				&window->session(),
				[=](not_null<PeerData*> peer) {
					if (!forwarder->setTarget(peer)) {
						window->showToast(
							tr::ayu_PrivateForwardTargetBad(tr::now));
						return;
					}
					if (const auto box = weak->get()) {
						box->closeBox();
					}
				});
			window->show(Box<PeerListBox>(
				std::unique_ptr<PeerListController>(std::move(picker)),
				[=](not_null<PeerListBox*> box) {
					*weak = box;
					box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
				}));
		},
	});
	builder.addDividerText(tr::ayu_PrivateForwardTargetAbout());

	builder.addSubsectionTitle(tr::ayu_PrivateForwardRules());
	const auto topics = builder.addButton({
		.id = u"ayu/privateForward/topics"_q,
		.title = tr::ayu_PrivateForwardPerPeer(),
		.st = &st::settingsButtonNoIcon,
	});
	BindToggle(topics, forwarder,
		[=] { return forwarder->settings().perPeerTopic; },
		[=](bool value) { forwarder->setPerPeerTopic(value); });
	builder.addDividerText(tr::ayu_PrivateForwardPerPeerAbout());
	builder.addButton({
		.id = u"ayu/privateForward/format"_q,
		.title = tr::ayu_PrivateForwardTopicFormat(),
		.st = &st::settingsButtonNoIcon,
		.label = LiveText(forwarder, [=] {
			return forwarder->settings().titleWithUsername
				? tr::ayu_PrivateForwardTopicNameUser(tr::now)
				: tr::ayu_PrivateForwardTopicName(tr::now);
		}),
		.onClick = [=] {
			const auto options = std::vector<QString>{
				tr::ayu_PrivateForwardTopicNameUser(tr::now),
				tr::ayu_PrivateForwardTopicName(tr::now),
			};
			window->show(Box([=](not_null<Ui::GenericBox*> box) {
				SingleChoiceBox(box, {
					.title = tr::ayu_PrivateForwardTopicFormat(),
					.options = options,
					.initialSelection = forwarder->settings().titleWithUsername
						? 0
						: 1,
					.callback = [=](int index) {
						forwarder->setTitleWithUsername(index == 0);
					},
				});
			}));
		},
	});

	builder.addSubsectionTitle(tr::ayu_PrivateForwardContent());
	builder.add([=](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		return {
			.widget = object_ptr<ContentRow>(ctx.container, forwarder),
			.margin = st::settingsCheckboxPadding,
		};
	});

	const auto copy = builder.addButton({
		.id = u"ayu/privateForward/copy"_q,
		.title = tr::ayu_PrivateForwardCopy(),
		.st = &st::settingsButtonNoIcon,
	});
	BindToggle(copy, forwarder,
		[=] { return forwarder->settings().copyWithoutHeader; },
		[=](bool value) { forwarder->setCopyWithoutHeader(value); });
	builder.addDividerText(tr::ayu_PrivateForwardCopyAbout());

	const auto own = builder.addButton({
		.id = u"ayu/privateForward/own"_q,
		.title = tr::ayu_PrivateForwardOwn(),
		.st = &st::settingsButtonNoIcon,
	});
	BindToggle(own, forwarder,
		[=] { return forwarder->settings().forwardOwn; },
		[=](bool value) { forwarder->setForwardOwn(value); });

	const auto hints = builder.addButton({
		.id = u"ayu/privateForward/hints"_q,
		.title = tr::ayu_PrivateForwardHints(),
		.st = &st::settingsButtonNoIcon,
	});
	BindToggle(hints, forwarder,
		[=] { return forwarder->settings().editDeleteHints; },
		[=](bool value) { forwarder->setEditDeleteHints(value); });
	builder.addDividerText(tr::ayu_PrivateForwardHintsAbout());

	builder.addSubsectionTitle(tr::ayu_PrivateForwardReply());
	const auto relay = builder.addButton({
		.id = u"ayu/privateForward/relay"_q,
		.title = tr::ayu_PrivateForwardReplyTitle(),
		.st = &st::settingsButtonNoIcon,
	});
	BindToggle(relay, forwarder,
		[=] { return forwarder->settings().relayReplies; },
		[=](bool value) {
			if (!value) {
				forwarder->setRelayReplies(false);
				return;
			}
			ConfirmEnable(
				window,
				forwarder,
				tr::ayu_PrivateForwardReplyWarn(),
				[=] { forwarder->setRelayReplies(true); });
		});
	builder.addDividerText(tr::ayu_PrivateForwardReplyAbout());

	const auto admins = builder.addButton({
		.id = u"ayu/privateForward/admins"_q,
		.title = tr::ayu_PrivateForwardAdmins(),
		.st = &st::settingsButtonNoIcon,
	});
	BindToggle(admins, forwarder,
		[=] { return forwarder->settings().allowOtherAdmins; },
		[=](bool value) {
			if (!value) {
				forwarder->setAllowOtherAdmins(false);
				return;
			}
			if (!forwarder->settings().relayReplies) {
				forwarder->touch();
				window->showToast(tr::ayu_PrivateForwardReplyTitle(tr::now));
				return;
			}
			ConfirmEnable(
				window,
				forwarder,
				tr::ayu_PrivateForwardAdminsWarn(),
				[=] { forwarder->setAllowOtherAdmins(true); });
		});
	builder.addDividerText(tr::ayu_PrivateForwardAdminsWarn());

	builder.addSubsectionTitle(tr::ayu_PrivateForwardExclude());
	const auto bots = builder.addButton({
		.id = u"ayu/privateForward/bots"_q,
		.title = tr::ayu_PrivateForwardIgnoreBots(),
		.st = &st::settingsButtonNoIcon,
	});
	BindToggle(bots, forwarder,
		[=] { return forwarder->settings().ignoreBots; },
		[=](bool value) { forwarder->setIgnoreBots(value); });
	const auto muted = builder.addButton({
		.id = u"ayu/privateForward/muted"_q,
		.title = tr::ayu_PrivateForwardIgnoreMuted(),
		.st = &st::settingsButtonNoIcon,
	});
	BindToggle(muted, forwarder,
		[=] { return forwarder->settings().ignoreMuted; },
		[=](bool value) { forwarder->setIgnoreMuted(value); });
	builder.addButton({
		.id = u"ayu/privateForward/excluded"_q,
		.title = tr::ayu_PrivateForwardExcludeList(),
		.st = &st::settingsButtonNoIcon,
		.label = LiveText(forwarder, [=] {
			return tr::ayu_PrivateForwardExcludeCount(
				tr::now,
				lt_count,
				int(forwarder->settings().excluded.size()));
		}),
		.onClick = [=] {
			auto picker = std::make_unique<ExcludeController>(
				&window->session(),
				forwarder);
			window->show(Box<PeerListBox>(
				std::unique_ptr<PeerListController>(std::move(picker)),
				[=](not_null<PeerListBox*> box) {
					box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });
				}));
		},
	});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
	builder.addDividerText(LiveText(forwarder, [=] {
		return tr::ayu_PrivateForwardToday(
				tr::now,
				lt_count,
				forwarder->todayCount())
			+ u"    "_q
			+ tr::ayu_PrivateForwardLastError(tr::now)
			+ u" "_q
			+ forwarder->lastErrorLabel();
	}));
	builder.addButton({
		.id = u"ayu/privateForward/log"_q,
		.title = tr::ayu_PrivateForwardLog(),
		.st = &st::settingsButtonNoIcon,
		.onClick = [=] {
			window->show(Box([=](not_null<Ui::GenericBox*> box) {
				box->setTitle(tr::ayu_PrivateForwardLogTitle());
				const auto rows = forwarder->logLines();
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
	.id = AyuPrivateForward::Id(),
	.parentId = AyuMain::Id(),
	.title = &tr::ayu_PrivateForwardTitle,
	.icon = &st::menuIconForward,
}, [](SectionBuilder &builder) {
	BuildPage(builder);
});

} // namespace

rpl::producer<QString> AyuPrivateForward::title() {
	return tr::ayu_PrivateForwardTitle();
}

AyuPrivateForward::AyuPrivateForward(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void AyuPrivateForward::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuPrivateForwardId() {
	return AyuPrivateForward::Id();
}

} // namespace Settings
