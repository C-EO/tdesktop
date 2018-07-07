/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/output/export_output_html.h"

#include "export/output/export_output_result.h"
#include "export/data/export_data_types.h"
#include "core/utils.h"

#include <QtCore/QFile>
#include <QtCore/QDateTime>

namespace Export {
namespace Output {
namespace {

constexpr auto kMessagesInFile = 1000;
constexpr auto kPersonalUserpicSize = 90;
constexpr auto kEntryUserpicSize = 48;
constexpr auto kServiceMessagePhotoSize = 60;
constexpr auto kHistoryUserpicSize = 42;
constexpr auto kSavedMessagesColorIndex = 3;
constexpr auto kJoinWithinSeconds = 900;

const auto kLineBreak = QByteArrayLiteral("<br>");

using Context = details::HtmlContext;
using UserpicData = details::UserpicData;
using PeersMap = details::PeersMap;
using MediaData = details::MediaData;

bool IsGlobalLink(const QString &link) {
	return link.startsWith(qstr("http://"), Qt::CaseInsensitive)
		|| link.startsWith(qstr("https://"), Qt::CaseInsensitive);
}

QByteArray SerializeString(const QByteArray &value) {
	const auto size = value.size();
	const auto begin = value.data();
	const auto end = begin + size;

	auto result = QByteArray();
	result.reserve(size * 6);
	for (auto p = begin; p != end; ++p) {
		const auto ch = *p;
		if (ch == '\n') {
			result.append("<br>", 4);
		} else if (ch == '"') {
			result.append("&quot;", 6);
		} else if (ch == '&') {
			result.append("&amp;", 5);
		} else if (ch == '\'') {
			result.append("&apos;", 6);
		} else if (ch == '<') {
			result.append("&lt;", 4);
		} else if (ch == '>') {
			result.append("&gt;", 4);
		} else if (ch >= 0 && ch < 32) {
			result.append("&#x", 3).append('0' + (ch >> 4));
			const auto left = (ch & 0x0F);
			if (left >= 10) {
				result.append('A' + (left - 10));
			} else {
				result.append('0' + left);
			}
			result.append(';');
		} else if (ch == char(0xE2)
			&& (p + 2 < end)
			&& *(p + 1) == char(0x80)) {
			if (*(p + 2) == char(0xA8)) { // Line separator.
				result.append("<br>", 4);
			} else if (*(p + 2) == char(0xA9)) { // Paragraph separator.
				result.append("<br>", 4);
			} else {
				result.append(ch);
			}
		} else {
			result.append(ch);
		}
	}
	return result;
}

QByteArray SerializeList(const std::vector<QByteArray> &values) {
	const auto count = values.size();
	if (count == 1) {
		return values[0];
	} else if (count > 1) {
		auto result = values[0];
		for (auto i = 1; i != count - 1; ++i) {
			result += ", " + values[i];
		}
		return result + " and " + values[count - 1];
	}
	return QByteArray();
}
QByteArray MakeLinks(const QByteArray &value) {
	const auto domain = QByteArray("https://telegram.org/");
	auto result = QByteArray();
	auto offset = 0;
	while (true) {
		const auto start = value.indexOf(domain, offset);
		if (start < 0) {
			break;
		}
		auto end = start + domain.size();
		for (; end != value.size(); ++end) {
			const auto ch = value[end];
			if ((ch < 'a' || ch > 'z')
				&& (ch < 'A' || ch > 'Z')
				&& (ch < '0' || ch > '9')
				&& (ch != '-')
				&& (ch != '_')
				&& (ch != '/')) {
				break;
			}
		}
		if (start > offset) {
			const auto link = value.mid(start, end - start);
			result.append(value.mid(offset, start - offset));
			result.append("<a href=\"").append(link).append("\">");
			result.append(link);
			result.append("</a>");
			offset = end;
		}
	}
	if (result.isEmpty()) {
		return value;
	}
	if (offset < value.size()) {
		result.append(value.mid(offset));
	}
	return result;
}

void SerializeMultiline(
		QByteArray &appendTo,
		const QByteArray &value,
		int newline) {
	const auto data = value.data();
	auto offset = 0;
	do {
		appendTo.append("> ");
		const auto win = (newline > 0 && *(data + newline - 1) == '\r');
		if (win) --newline;
		appendTo.append(data + offset, newline - offset).append(kLineBreak);
		if (win) ++newline;
		offset = newline + 1;
		newline = value.indexOf('\n', offset);
	} while (newline > 0);
	if (const auto size = value.size(); size > offset) {
		appendTo.append("> ");
		appendTo.append(data + offset, size - offset).append(kLineBreak);
	}
}

QByteArray JoinList(
		const QByteArray &separator,
		const std::vector<QByteArray> &list) {
	if (list.empty()) {
		return QByteArray();
	} else if (list.size() == 1) {
		return list[0];
	}
	auto size = (list.size() - 1) * separator.size();
	for (const auto &value : list) {
		size += value.size();
	}
	auto result = QByteArray();
	result.reserve(size);
	auto counter = 0;
	while (true) {
		result.append(list[counter]);
		if (++counter == list.size()) {
			break;
		} else {
			result.append(separator);
		}
	}
	return result;
}

QByteArray FormatText(
		const std::vector<Data::TextPart> &data,
		const QString &internalLinksDomain) {
	return JoinList(QByteArray(), ranges::view::all(
		data
	) | ranges::view::transform([&](const Data::TextPart &part) {
		const auto text = SerializeString(part.text);
		using Type = Data::TextPart::Type;
		switch (part.type) {
		case Type::Text: return text;
		case Type::Unknown: return text;
		case Type::Mention:
			return "<a href=\""
				+ internalLinksDomain.toUtf8()
				+ text.mid(1)
				+ "\">" + text + "</a>";
		case Type::Hashtag: return "<a href=\"#hash-"
			+ text.mid(1)
			+ "\">" + text + "</a>";
		case Type::BotCommand: return "<a href=\"#command-"
			+ text.mid(1)
			+ "\">" + text + "</a>";
		case Type::Url: return "<a href=\""
			+ text
			+ "\">" + text + "</a>";
		case Type::Email: return "<a href=\"mailto:"
			+ text
			+ "\">" + text + "</a>";
		case Type::Bold: return "<strong>" + text + "</strong>";
		case Type::Italic: return "<em>" + text + "</em>";
		case Type::Code: return "<code>" + text + "</code>";
		case Type::Pre: return "<pre>" + text + "</pre>";
		case Type::TextUrl: return "<a href=\""
			+ SerializeString(part.additional)
			+ "\">" + text + "</a>";
		case Type::MentionName: return "<a href=\"#mention-"
			+ part.additional
			+ "\">" + text + "</a>";
		case Type::Phone: return "<a href=\"tel:"
			+ text
			+ "\">" + text + "</a>";
		case Type::Cashtag: return "<a href=\"#cash-"
			+ text.mid(1)
			+ "\">" + text + "</a>";
		}
		Unexpected("Type in text entities serialization.");
	}) | ranges::to_vector);
}

QByteArray SerializeKeyValue(
		std::vector<std::pair<QByteArray, QByteArray>> &&values) {
	auto result = QByteArray();
	for (const auto &[key, value] : values) {
		if (value.isEmpty()) {
			continue;
		}
		result.append(key);
		if (const auto newline = value.indexOf('\n'); newline >= 0) {
			result.append(':').append(kLineBreak);
			SerializeMultiline(result, value, newline);
		} else {
			result.append(": ").append(value).append(kLineBreak);
		}
	}
	return result;
}

QByteArray SerializeBlockquote(
		std::vector<std::pair<QByteArray, QByteArray>> &&values) {
	return "<blockquote>"
		+ SerializeKeyValue(std::move(values))
		+ "</blockquote>";
}

Data::Utf8String FormatUsername(const Data::Utf8String &username) {
	return username.isEmpty() ? username : ('@' + username);
}

bool DisplayDate(TimeId date, TimeId previousDate) {
	if (!previousDate) {
		return true;
	}
	return QDateTime::fromTime_t(date).date()
		!= QDateTime::fromTime_t(previousDate).date();
}

QByteArray FormatDateText(TimeId date) {
	const auto parsed = QDateTime::fromTime_t(date).date();
	const auto month = [](int index) {
		switch (index) {
		case 1: return "January";
		case 2: return "February";
		case 3: return "March";
		case 4: return "April";
		case 5: return "May";
		case 6: return "June";
		case 7: return "July";
		case 8: return "August";
		case 9: return "September";
		case 10: return "October";
		case 11: return "November";
		case 12: return "December";
		}
		return "Unknown";
	};
	return Data::NumberToString(parsed.day())
		+ ' '
		+ month(parsed.month())
		+ ' '
		+ Data::NumberToString(parsed.year());
}

QByteArray FormatTimeText(TimeId date) {
	const auto parsed = QDateTime::fromTime_t(date).time();
	return Data::NumberToString(parsed.hour(), 2)
		+ ':'
		+ Data::NumberToString(parsed.minute(), 2);
}

QByteArray SerializeLink(
		const Data::Utf8String &text,
		const QString &path) {
	return "<a href=\"" + path.toUtf8() + "\">" + text + "</a>";
}

} // namespace

namespace details {

struct UserpicData {
	int colorIndex = 0;
	int pixelSize = 0;
	QString imageLink;
	QString largeLink;
	QByteArray firstName;
	QByteArray lastName;
};

class PeersMap {
public:
	using PeerId = Data::PeerId;
	using Peer = Data::Peer;
	using User = Data::User;
	using Chat = Data::Chat;

	PeersMap(const std::map<PeerId, Peer> &data);

	const Peer &peer(PeerId peerId) const;
	const User &user(int32 userId) const;
	const Chat &chat(int32 chatId) const;

	QByteArray wrapPeerName(PeerId peerId) const;
	QByteArray wrapUserName(int32 userId) const;
	QByteArray wrapUserNames(const std::vector<int32> &data) const;

private:
	const std::map<Data::PeerId, Data::Peer> &_data;

};

struct MediaData {
	QByteArray title;
	QByteArray description;
	QByteArray status;
	QByteArray classes;
	QString thumb;
	QString link;
};

PeersMap::PeersMap(const std::map<PeerId, Peer> &data) : _data(data) {
}

auto PeersMap::peer(PeerId peerId) const -> const Peer & {
	if (const auto i = _data.find(peerId); i != end(_data)) {
		return i->second;
	}
	static auto empty = Peer{ User() };
	return empty;
}

auto PeersMap::user(int32 userId) const -> const User & {
	if (const auto result = peer(Data::UserPeerId(userId)).user()) {
		return *result;
	}
	static auto empty = User();
	return empty;
}

auto PeersMap::chat(int32 chatId) const -> const Chat & {
	if (const auto result = peer(Data::ChatPeerId(chatId)).chat()) {
		return *result;
	}
	static auto empty = Chat();
	return empty;
}

QByteArray PeersMap::wrapPeerName(PeerId peerId) const {
	const auto result = peer(peerId).name();
	return result.isEmpty()
		? QByteArray("Deleted")
		: SerializeString(result);
}

QByteArray PeersMap::wrapUserName(int32 userId) const {
	const auto result = user(userId).name();
	return result.isEmpty()
		? QByteArray("Deleted Account")
		: SerializeString(result);
}

QByteArray PeersMap::wrapUserNames(const std::vector<int32> &data) const {
	auto list = std::vector<QByteArray>();
	for (const auto userId : data) {
		list.push_back(wrapUserName(userId));
	}
	return SerializeList(list);
}

QByteArray HtmlContext::pushTag(
		const QByteArray &tag,
		std::map<QByteArray, QByteArray> &&attributes) {
	auto data = Tag();
	data.name = tag;
	auto empty = false;
	auto inner = QByteArray();
	for (const auto &[name, value] : attributes) {
		if (name == "inline") {
			data.block = false;
		} else if (name == "empty") {
			empty = true;
		} else {
			inner.append(' ').append(name);
			inner.append("=\"").append(SerializeString(value)).append("\"");
		}
	}
	auto result = (data.block ? ("\n" + indent()) : QByteArray())
		+ "<" + data.name + inner + (empty ? "/" : "") + ">"
		+ (data.block ? "\n" : "");
	if (!empty) {
		_tags.push_back(data);
	}
	return result;
}

QByteArray HtmlContext::popTag() {
	Expects(!_tags.empty());

	const auto data = _tags.back();
	_tags.pop_back();
	return (data.block ? ("\n" + indent()) : QByteArray())
		+ "</" + data.name + ">"
		+ (data.block ? "\n" : "");
}

QByteArray HtmlContext::indent() const {
	return QByteArray(_tags.size(), ' ');
}

bool HtmlContext::empty() const {
	return _tags.empty();
}

} // namespace details

struct HtmlWriter::MessageInfo {
	enum class Type {
		Service,
		Default,
	};
	Type type = Type::Service;
	int32 fromId = 0;
	TimeId date = 0;
	Data::PeerId forwardedFromId = 0;
	TimeId forwardedDate = 0;
};

class HtmlWriter::Wrap {
public:
	Wrap(const QString &path, const QString &base, Stats *stats);

	[[nodiscard]] bool empty() const;

	[[nodiscard]] QByteArray pushTag(
		const QByteArray &tag,
		std::map<QByteArray, QByteArray> &&attributes = {});
	[[nodiscard]] QByteArray popTag();
	[[nodiscard]] QByteArray indent() const;

	[[nodiscard]] QByteArray pushDiv(
		const QByteArray &className,
		const QByteArray &style = {});

	[[nodiscard]] QByteArray pushUserpic(const UserpicData &userpic);
	[[nodiscard]] QByteArray pushListEntry(
		const UserpicData &userpic,
		const QByteArray &name,
		const QByteArray &details,
		const QByteArray &info,
		const QString &link = QString());
	[[nodiscard]] QByteArray pushSessionListEntry(
		int apiId,
		const QByteArray &name,
		const QByteArray &subname,
		std::initializer_list<QByteArray> details,
		const QByteArray &info = QByteArray());

	[[nodiscard]] QByteArray pushHeader(
		const QByteArray &header,
		const QString &path = QString());
	[[nodiscard]] QByteArray pushSection(
		const QByteArray &label,
		const QByteArray &type,
		int count,
		const QString &path);
	[[nodiscard]] QByteArray pushAbout(
		const QByteArray &text,
		bool withDivider = false);
	[[nodiscard]] QByteArray pushServiceMessage(
		int messageId,
		const Data::DialogInfo &dialog,
		const QString &basePath,
		const QByteArray &text,
		const Data::Photo *photo = nullptr);
	[[nodiscard]] std::pair<MessageInfo, QByteArray> pushMessage(
		const Data::Message &message,
		const MessageInfo *previous,
		const Data::DialogInfo &dialog,
		const QString &basePath,
		const PeersMap &peers,
		const QString &internalLinksDomain);
	[[nodiscard]] QByteArray pushMedia(
		const Data::Message &message,
		const QString &basePath,
		const PeersMap &peers,
		const QString &internalLinksDomain);

	[[nodiscard]] Result writeBlock(const QByteArray &block);

	[[nodiscard]] Result close();

	[[nodiscard]] QString relativePath(const QString &path) const;
	[[nodiscard]] QString relativePath(const Data::File &file) const;

	~Wrap();

private:
	[[nodiscard]] QByteArray composeStart();
	[[nodiscard]] QByteArray pushGenericListEntry(
		const QString &link,
		const UserpicData &userpic,
		const QByteArray &name,
		const QByteArray &subname,
		std::initializer_list<QByteArray> details,
		const QByteArray &info);

	[[nodiscard]] bool messageNeedsWrap(
		const Data::Message &message,
		const MessageInfo *previous) const;
	[[nodiscard]] bool forwardedNeedsWrap(
		const Data::Message &message,
		const MessageInfo *previous) const;

	[[nodiscard]] MediaData prepareMediaData(
		const Data::Message &message,
		const QString &basePath,
		const PeersMap &peers,
		const QString &internalLinksDomain) const;

	File _file;
	bool _closed = false;
	QByteArray _base;
	Context _context;

};

struct HtmlWriter::SavedSection {
	int priority = 0;
	QByteArray label;
	QByteArray type;
	int count = 0;
	QString path;
};

void FillUserpicNames(UserpicData &data, const Data::Peer &peer) {
	if (peer.user()) {
		data.firstName = peer.user()->info.firstName;
		data.lastName = peer.user()->info.lastName;
	} else if (peer.chat()) {
		data.firstName = peer.name();
	}
}

QByteArray ComposeName(const UserpicData &data, const QByteArray &empty) {
	return ((data.firstName.isEmpty() && data.lastName.isEmpty())
		? empty
		: (data.firstName + ' ' + data.lastName));
}

QString WriteUserpicThumb(
		const QString &basePath,
		const QString &largePath,
		const UserpicData &userpic,
		const QString &postfix = "_thumb") {
	return Data::WriteImageThumb(
		basePath,
		largePath,
		userpic.pixelSize * 2,
		userpic.pixelSize * 2,
		postfix);
}

HtmlWriter::Wrap::Wrap(
	const QString &path,
	const QString &base,
	Stats *stats)
: _file(path, stats) {
	Expects(base.endsWith('/'));
	Expects(path.startsWith(base));

	const auto left = path.mid(base.size());
	const auto nesting = ranges::count(left, '/');
	_base = QString("../").repeated(nesting).toUtf8();
}

bool HtmlWriter::Wrap::empty() const {
	return _file.empty();
}

QByteArray HtmlWriter::Wrap::pushTag(
		const QByteArray &tag,
		std::map<QByteArray, QByteArray> &&attributes) {
	return _context.pushTag(tag, std::move(attributes));
}

QByteArray HtmlWriter::Wrap::popTag() {
	return _context.popTag();
}

QByteArray HtmlWriter::Wrap::indent() const {
	return _context.indent();
}

QByteArray HtmlWriter::Wrap::pushDiv(
		const QByteArray &className,
		const QByteArray &style) {
	return style.isEmpty()
		? _context.pushTag("div", { { "class", className } })
		: _context.pushTag("div", {
			{ "class", className },
			{ "style", style }
		});
}

QByteArray HtmlWriter::Wrap::pushUserpic(const UserpicData &userpic) {
	const auto size = Data::NumberToString(userpic.pixelSize) + "px";
	auto result = QByteArray();
	if (!userpic.largeLink.isEmpty()) {
		result.append(pushTag("a", {
			{ "class", "userpic_link" },
			{ "href", relativePath(userpic.largeLink).toUtf8() }
		}));
	}
	const auto sizeStyle = "width: " + size + "; height: " + size;
	if (!userpic.imageLink.isEmpty()) {
		result.append(pushTag("img", {
			{ "class", "userpic" },
			{ "style", sizeStyle },
			{ "src", relativePath(userpic.imageLink).toUtf8() },
			{ "empty", "" }
		}));
	} else {
		result.append(pushTag("div", {
			{
				"class",
				"userpic userpic"
				+ Data::NumberToString(userpic.colorIndex + 1)
			},
			{ "style", sizeStyle }
		}));
		result.append(pushDiv(
			"initials",
			"line-height: " + size));
		auto character = [](const QByteArray &from) {
			const auto utf = QString::fromUtf8(from).trimmed();
			return utf.isEmpty() ? QByteArray() : utf.mid(0, 1).toUtf8();
		};
		result.append(character(userpic.firstName));
		result.append(character(userpic.lastName));
		result.append(popTag());
		result.append(popTag());
	}
	if (!userpic.largeLink.isEmpty()) {
		result.append(popTag());
	}
	return result;
}

QByteArray HtmlWriter::Wrap::pushListEntry(
		const UserpicData &userpic,
		const QByteArray &name,
		const QByteArray &details,
		const QByteArray &info,
		const QString &link) {
	return pushGenericListEntry(
		link,
		userpic,
		name,
		{},
		{ details },
		info);
}

QByteArray HtmlWriter::Wrap::pushSessionListEntry(
		int apiId,
		const QByteArray &name,
		const QByteArray &subname,
		std::initializer_list<QByteArray> details,
		const QByteArray &info) {
	const auto link = QString();
	auto userpic = UserpicData{
		Data::ApplicationColorIndex(apiId),
		kEntryUserpicSize
	};
	userpic.firstName = name;
	return pushGenericListEntry(
		link,
		userpic,
		name,
		subname,
		details,
		info);
}

QByteArray HtmlWriter::Wrap::pushGenericListEntry(
		const QString &link,
		const UserpicData &userpic,
		const QByteArray &name,
		const QByteArray &subname,
		std::initializer_list<QByteArray> details,
		const QByteArray &info) {
	auto result = link.isEmpty()
		? pushDiv("entry clearfix")
		: pushTag("a", {
			{ "class", "entry block_link clearfix" },
			{ "href", relativePath(link).toUtf8() },
		});
	result.append(pushDiv("pull_left userpic_wrap"));
	result.append(pushUserpic(userpic));
	result.append(popTag());
	result.append(pushDiv("body"));
	if (!info.isEmpty()) {
		result.append(pushDiv("pull_right info details"));
		result.append(SerializeString(info));
		result.append(popTag());
	}
	if (!name.isEmpty()) {
		result.append(pushDiv("name bold"));
		result.append(SerializeString(name));
		result.append(popTag());
	}
	if (!subname.isEmpty()) {
		result.append(pushDiv("subname bold"));
		result.append(SerializeString(subname));
		result.append(popTag());
	}
	for (const auto detail : details) {
		result.append(pushDiv("details_entry details"));
		result.append(SerializeString(detail));
		result.append(popTag());
	}
	result.append(popTag());
	result.append(popTag());
	return result;
}

Result HtmlWriter::Wrap::writeBlock(const QByteArray &block) {
	Expects(!_closed);

	const auto result = [&] {
		if (block.isEmpty()) {
			return _file.writeBlock(block);
		} else if (_file.empty()) {
			return _file.writeBlock(composeStart() + block);
		}
		return _file.writeBlock(block);
	}();
	if (!result) {
		_closed = true;
	}
	return result;
}

QByteArray HtmlWriter::Wrap::pushHeader(
		const QByteArray &header,
		const QString &path) {
	auto result = pushDiv("page_header");
	result.append(path.isEmpty()
		? pushDiv("content")
		: pushTag("a", {
			{ "class", "content block_link" },
			{ "href", relativePath(path).toUtf8() }
		}));
	result.append(pushDiv("text bold"));
	result.append(SerializeString(header));
	result.append(popTag());
	result.append(popTag());
	result.append(popTag());
	return result;
}

QByteArray HtmlWriter::Wrap::pushSection(
		const QByteArray &header,
		const QByteArray &type,
		int count,
		const QString &link) {
	auto result = pushTag("a", {
		{ "class", "section block_link " + type },
		{ "href", link.toUtf8() },
	});
	result.append(pushDiv("counter details"));
	result.append(Data::NumberToString(count));
	result.append(popTag());
	result.append(pushDiv("label bold"));
	result.append(SerializeString(header));
	result.append(popTag());
	result.append(popTag());
	return result;
}

QByteArray HtmlWriter::Wrap::pushAbout(
		const QByteArray &text,
		bool withDivider) {
	auto result = pushDiv(withDivider
		? "page_about details with_divider"
		: "page_about details");
	result.append(MakeLinks(SerializeString(text)));
	result.append(popTag());
	return result;
}

QByteArray HtmlWriter::Wrap::pushServiceMessage(
		int messageId,
		const Data::DialogInfo &dialog,
		const QString &basePath,
		const QByteArray &serialized,
		const Data::Photo *photo) {
	auto result = pushTag("div", {
		{ "class", "message service" },
		{ "id", "message" + Data::NumberToString(messageId) }
	});
	result.append(pushDiv("body details"));
	result.append(serialized);
	result.append(popTag());
	if (photo) {
		auto userpic = UserpicData();
		userpic.colorIndex = Data::PeerColorIndex(
			Data::BarePeerId(dialog.peerId));
		userpic.firstName = dialog.name;
		userpic.lastName = dialog.lastName;
		userpic.pixelSize = kServiceMessagePhotoSize;
		userpic.largeLink = photo->image.file.relativePath;
		userpic.imageLink = WriteUserpicThumb(
			basePath,
			userpic.largeLink,
			userpic);
		result.append(pushDiv("userpic_wrap"));
		result.append(pushUserpic(userpic));
		result.append(popTag());
	}
	result.append(popTag());
	return result;
}

auto HtmlWriter::Wrap::pushMessage(
		const Data::Message &message,
		const MessageInfo *previous,
		const Data::DialogInfo &dialog,
		const QString &basePath,
		const PeersMap &peers,
		const QString &internalLinksDomain
) -> std::pair<MessageInfo, QByteArray> {
	using namespace Data;

	auto info = MessageInfo();
	info.fromId = message.fromId;
	info.date = message.date;
	info.forwardedFromId = message.forwardedFromId;
	info.forwardedDate = message.forwardedDate;
	if (message.media.content.is<UnsupportedMedia>()) {
		return { info, pushServiceMessage(
			message.id,
			dialog,
			basePath,
			"This message is not supported by this version "
			"of Telegram Desktop. Please update the application.") };
	}

	const auto wrapReplyToLink = [&](const QByteArray &text) {
		return "<a href=\"#message"
			+ NumberToString(message.replyToMsgId)
			+ "\">"
			+ text + "</a>";
	};

	const auto serviceFrom = peers.wrapUserName(message.fromId);
	const auto serviceText = message.action.content.match(
	[&](const ActionChatCreate &data) {
		return serviceFrom
			+ " created group &laquo;" + data.title + "&raquo;"
			+ (data.userIds.empty()
				? QByteArray()
				: " with members " + peers.wrapUserNames(data.userIds));
	}, [&](const ActionChatEditTitle &data) {
		return serviceFrom
			+ " changed group title to &laquo;" + data.title + "&raquo;";
	}, [&](const ActionChatEditPhoto &data) {
		return serviceFrom
			+ " changed group photo";
	}, [&](const ActionChatDeletePhoto &data) {
		return serviceFrom
			+ " deleted group photo";
	}, [&](const ActionChatAddUser &data) {
		return serviceFrom
			+ " invited "
			+ peers.wrapUserNames(data.userIds);
	}, [&](const ActionChatDeleteUser &data) {
		return serviceFrom
			+ " removed "
			+ peers.wrapUserName(data.userId);
	}, [&](const ActionChatJoinedByLink &data) {
		return serviceFrom
			+ " joined group by link from "
			+ peers.wrapUserName(data.inviterId);
	}, [&](const ActionChannelCreate &data) {
		return "Channel &laquo;" + data.title + "&raquo; created";
	}, [&](const ActionChatMigrateTo &data) {
		return serviceFrom
			+ " converted this group to a supergroup";
	}, [&](const ActionChannelMigrateFrom &data) {
		return serviceFrom
			+ " converted a basic group to this supergroup "
			+ "&laquo;" + data.title + "&raquo;";
	}, [&](const ActionPinMessage &data) {
		return serviceFrom
			+ " pinned "
			+ wrapReplyToLink("this message");
	}, [&](const ActionHistoryClear &data) {
		return QByteArray("History cleared");
	}, [&](const ActionGameScore &data) {
		return serviceFrom
			+ " scored "
			+ NumberToString(data.score)
			+ " in "
			+ wrapReplyToLink("this game");
	}, [&](const ActionPaymentSent &data) {
		return "You have successfully transferred "
			+ FormatMoneyAmount(data.amount, data.currency)
			+ " for "
			+ wrapReplyToLink("this invoice");
	}, [&](const ActionPhoneCall &data) {
		return QByteArray();
	}, [&](const ActionScreenshotTaken &data) {
		return serviceFrom + " took a screenshot";
	}, [&](const ActionCustomAction &data) {
		return data.message;
	}, [&](const ActionBotAllowed &data) {
		return "You allowed this bot to message you when you logged in on "
			+ data.domain;
	}, [&](const ActionSecureValuesSent &data) {
		auto list = std::vector<QByteArray>();
		for (const auto type : data.types) {
			list.push_back([&] {
				using Type = ActionSecureValuesSent::Type;
				switch (type) {
				case Type::PersonalDetails: return "Personal details";
				case Type::Passport: return "Passport";
				case Type::DriverLicense: return "Driver license";
				case Type::IdentityCard: return "Identity card";
				case Type::InternalPassport: return "Internal passport";
				case Type::Address: return "Address information";
				case Type::UtilityBill: return "Utility bill";
				case Type::BankStatement: return "Bank statement";
				case Type::RentalAgreement: return "Rental agreement";
				case Type::PassportRegistration:
					return "Passport registration";
				case Type::TemporaryRegistration:
					return "Temporary registration";
				case Type::Phone: return "Phone number";
				case Type::Email: return "Email";
				}
				return "";
			}());
		}
		return "You have sent the following documents: "
			+ SerializeList(list);
	}, [](const base::none_type &) { return QByteArray(); });

	if (!serviceText.isEmpty()) {
		const auto &content = message.action.content;
		const auto photo = content.is<ActionChatEditPhoto>()
			? &content.get_unchecked<ActionChatEditPhoto>().photo
			: nullptr;
		return { info, pushServiceMessage(
			message.id,
			dialog,
			basePath,
			serviceText,
			photo) };
	}
	info.type = MessageInfo::Type::Default;

	const auto wrap = messageNeedsWrap(message, previous);
	const auto fromPeerId = message.fromId
		? UserPeerId(message.fromId)
		: ChatPeerId(message.chatId);
	auto userpic = UserpicData();
	userpic.colorIndex = PeerColorIndex(BarePeerId(fromPeerId));
	userpic.pixelSize = kHistoryUserpicSize;
	FillUserpicNames(userpic, peers.peer(fromPeerId));

	const auto via = [&] {
		if (message.viaBotId) {
			const auto &user = peers.user(message.viaBotId);
			if (!user.username.isEmpty()) {
				return SerializeString(user.username);
			}
		}
		return QByteArray();
	}();

	const auto className = wrap
		? "message default clearfix"
		: "message default clearfix joined";
	auto block = pushTag("div", {
		{ "class", className },
		{ "id", "message" + NumberToString(message.id) }
	});
	if (wrap) {
		block.append(pushDiv("pull_left userpic_wrap"));
		block.append(pushUserpic(userpic));
		block.append(popTag());
	}
	block.append(pushDiv("body"));
	block.append(pushTag("div", {
		{ "class", "pull_right date details" },
		{ "title", FormatDateTime(message.date) },
	}));
	block.append(FormatTimeText(message.date));
	block.append(popTag());
	if (wrap) {
		block.append(pushDiv("from_name"));
		block.append(SerializeString(
			ComposeName(userpic, "Deleted Account")));
		if (!via.isEmpty() && !message.forwardedFromId) {
			block.append(" via @" + via);
		}
		block.append(popTag());
	}
	if (message.forwardedFromId) {
		auto forwardedUserpic = UserpicData();
		forwardedUserpic.colorIndex = PeerColorIndex(
			BarePeerId(message.forwardedFromId));
		forwardedUserpic.pixelSize = kHistoryUserpicSize;
		FillUserpicNames(
			forwardedUserpic,
			peers.peer(message.forwardedFromId));

		const auto forwardedWrap = forwardedNeedsWrap(message, previous);
		if (forwardedWrap) {
			block.append(pushDiv("pull_left forwarded userpic_wrap"));
			block.append(pushUserpic(forwardedUserpic));
			block.append(popTag());
		}
		block.append(pushDiv("forwarded body"));
		if (forwardedWrap) {
			block.append(pushDiv("from_name"));
			block.append(SerializeString(
				ComposeName(forwardedUserpic, "Deleted Account")));
			if (!via.isEmpty()) {
				block.append(" via @" + via);
			}
			block.append(pushTag("span", {
				{ "class", "details" },
				{ "inline", "" }
			}));
			block.append(' ' + FormatDateTime(message.forwardedDate));
			block.append(popTag());
			block.append(popTag());
		}
	}
	if (message.replyToMsgId) {
		block.append(pushDiv("reply_to details"));
		block.append("In reply to ");
		block.append(wrapReplyToLink("this message"));
		block.append(popTag());
	}

	block.append(pushMedia(message, basePath, peers, internalLinksDomain));

	const auto text = FormatText(message.text, internalLinksDomain);
	if (!text.isEmpty()) {
		block.append(pushDiv("text"));
		block.append(text);
		block.append(popTag());
	}
	if (!message.signature.isEmpty()) {
		block.append(pushDiv("signature details"));
		block.append(SerializeString(message.signature));
		block.append(popTag());
	}
	if (message.forwardedFromId) {
		block.append(popTag());
	}
	block.append(popTag());
	block.append(popTag());

	return { info, block };
}

bool HtmlWriter::Wrap::messageNeedsWrap(
		const Data::Message &message,
		const MessageInfo *previous) const {
	if (!previous) {
		return true;
	} else if (previous->type != MessageInfo::Type::Default) {
		return true;
	} else if (!message.fromId || previous->fromId != message.fromId) {
		return true;
	} else if (QDateTime::fromTime_t(previous->date).date()
		!= QDateTime::fromTime_t(message.date).date()) {
		return true;
	} else if (!message.forwardedFromId != !previous->forwardedFromId) {
		return true;
	} else if (std::abs(message.date - previous->date)
		> (message.forwardedFromId ? 1 : kJoinWithinSeconds)) {
		return true;
	}
	return false;
}

QByteArray HtmlWriter::Wrap::pushMedia(
		const Data::Message &message,
		const QString &basePath,
		const PeersMap &peers,
		const QString &internalLinksDomain) {
	const auto data = prepareMediaData(
		message,
		basePath,
		peers,
		internalLinksDomain);
	if (data.classes.isEmpty()) {
		return QByteArray();
	}
	auto result = pushDiv("media_wrap clearfix");
	if (data.link.isEmpty()) {
		result.append(pushDiv("media clearfix pull_left " + data.classes));
	} else {
		result.append(pushTag("a", {
			{
				"class",
				"media clearfix pull_left block_link " + data.classes
			},
			{
				"href",
				(IsGlobalLink(data.link)
					? data.link.toUtf8()
					: relativePath(data.link).toUtf8())
			}
		}));
	}
	if (data.thumb.isEmpty()) {
		result.append(pushDiv("fill pull_left"));
		result.append(popTag());
	} else {
		result.append(pushTag("img", {
			{ "class", "thumb pull_left" },
			{ "src", relativePath(data.thumb).toUtf8() },
			{ "empty", "" }
		}));
	}
	result.append(pushDiv("body"));
	if (!data.title.isEmpty()) {
		result.append(pushDiv("title bold"));
		result.append(SerializeString(data.title));
		result.append(popTag());
	}
	if (!data.description.isEmpty()) {
		result.append(pushDiv("description"));
		result.append(SerializeString(data.description));
		result.append(popTag());
	}
	if (!data.status.isEmpty()) {
		result.append(pushDiv("status details"));
		result.append(SerializeString(data.status));
		result.append(popTag());
	}
	result.append(popTag());
	result.append(popTag());
	result.append(popTag());
	return result;
}

MediaData HtmlWriter::Wrap::prepareMediaData(
		const Data::Message &message,
		const QString &basePath,
		const PeersMap &peers,
		const QString &internalLinksDomain) const {
	using namespace Data;

	auto result = MediaData();
	const auto &action = message.action;
	if (const auto call = base::get_if<ActionPhoneCall>(&action.content)) {
		result.classes = "media_call";
		result.title = peers.peer(message.toId).name();
		result.status = [&] {
			using Reason = ActionPhoneCall::DiscardReason;
			const auto reason = call->discardReason;
			if (message.out) {
				return reason == Reason::Missed ? "Cancelled" : "Outgoing";
			} else if (reason == Reason::Missed) {
				return "Missed";
			} else if (reason == Reason::Busy) {
				return "Declined";
			}
			return "Incoming";
		}();
		if (call->duration > 0) {
			result.classes += " success";
			result.status += " ("
				+ NumberToString(call->duration)
				+ " seconds)";
		}
		return result;
	}

	message.media.content.match([&](const Photo &photo) {
		// #TODO export: photo + self destruct (ttl)
		result.title = "Photo";
		result.status = NumberToString(photo.image.width)
			+ "x"
			+ NumberToString(photo.image.height);
		result.classes = "media_file"; // #TODO export
		result.link = photo.image.file.relativePath;
	}, [&](const Document &data) {
		// #TODO export: sticker + thumb (video, video message) + self destruct (ttl)
		result.link = data.file.relativePath;
		if (data.isSticker) {
			result.title = "Sticker";
			result.status = data.stickerEmoji;
			result.classes = "media_file"; // #TODO export
		} else if (data.isVideoMessage) {
			result.title = "Video message";
			result.status = FormatDuration(data.duration);
			result.thumb = data.thumb.file.relativePath;
			result.classes = "media_file";
		} else if (data.isVoiceMessage) {
			result.title = "Voice message";
			result.status = FormatDuration(data.duration);
			result.classes = "media_voice_message";
		} else if (data.isAnimated) {
			result.title = "Animation";
			result.status = FormatFileSize(data.duration);
			result.classes = "media_file"; // #TODO export
		} else if (data.isVideoFile) {
			result.title = "Video file";
			result.status = FormatDuration(data.duration);
			result.classes = "media_file"; // #TODO export
		} else if (data.isAudioFile) {
			result.title = (data.songPerformer.isEmpty()
				|| data.songTitle.isEmpty())
				? QByteArray("Audio file")
				: data.songPerformer + " \xe2\x80\x93 " + data.songTitle;
			result.status = FormatDuration(data.duration);
			result.classes = "media_audio_file";
		} else {
			result.title = data.name.isEmpty()
				? QByteArray("File")
				: data.name;
			result.status = FormatFileSize(data.duration);
			result.classes = "media_file";
		}
	}, [&](const SharedContact &data) {
		result.title = data.info.firstName + ' ' + data.info.lastName;
		result.classes = "media_contact";
		result.status = FormatPhoneNumber(data.info.phoneNumber);
		if (!data.vcard.content.isEmpty()) {
			result.status += " - vCard";
			result.link = data.vcard.relativePath;
		}
	}, [&](const GeoPoint &data) {
		if (message.media.ttl) {
			result.classes = "media_live_location";
			result.title = "Live location";
			result.status = "";
		} else {
			result.classes = "media_location";
			result.title = "Location";
		}
		if (data.valid) {
			const auto latitude = NumberToString(data.latitude);
			const auto longitude = NumberToString(data.longitude);
			const auto coords = latitude + ',' + longitude;
			result.status = latitude + ", " + longitude;
			result.link = "https://maps.google.com/maps?q="
				+ coords
				+ "&ll="
				+ coords
				+ "&z=16";
		}
	}, [&](const Venue &data) {
		result.classes = "media_venue";
		result.title = data.title;
		result.description = data.address;
		if (data.point.valid) {
			const auto latitude = NumberToString(data.point.latitude);
			const auto longitude = NumberToString(data.point.longitude);
			const auto coords = latitude + ',' + longitude;
			result.link = "https://maps.google.com/maps?q="
				+ coords
				+ "&ll="
				+ coords
				+ "&z=16";
		}
	}, [&](const Game &data) {
		result.classes = "media_game";
		result.title = data.title;
		result.description = data.description;
		if (data.botId != 0 && !data.shortName.isEmpty()) {
			const auto bot = peers.user(data.botId);
			if (bot.isBot && !bot.username.isEmpty()) {
				const auto link = internalLinksDomain.toUtf8()
					+ bot.username
					+ "?game="
					+ data.shortName;
				result.link = link;
				result.status = link;
			}
		}
	}, [&](const Invoice &data) {
		result.classes = "media_invoice";
		result.title = data.title;
		result.description = data.description;
		result.status = Data::FormatMoneyAmount(data.amount, data.currency);
	}, [](const UnsupportedMedia &data) {
		Unexpected("Unsupported message.");
	}, [](const base::none_type &) {});
	return result;
}

bool HtmlWriter::Wrap::forwardedNeedsWrap(
		const Data::Message &message,
		const MessageInfo *previous) const {
	Expects(message.forwardedFromId != 0);

	if (messageNeedsWrap(message, previous)) {
		return true;
	} else if (message.forwardedFromId != previous->forwardedFromId) {
		return true;
	} else if (Data::IsChatPeerId(message.forwardedFromId)) {
		return true;
	} else if (abs(message.forwardedDate - previous->forwardedDate)
		> kJoinWithinSeconds) {
		return true;
	}
	return false;
}

Result HtmlWriter::Wrap::close() {
	if (!std::exchange(_closed, true) && !_file.empty()) {
		auto block = QByteArray();
		while (!_context.empty()) {
			block.append(_context.popTag());
		}
		return _file.writeBlock(block);
	}
	return Result::Success();
}

QString HtmlWriter::Wrap::relativePath(const QString &path) const {
	return _base + path;
}

QString HtmlWriter::Wrap::relativePath(const Data::File &file) const {
	return relativePath(file.relativePath);
}

QByteArray HtmlWriter::Wrap::composeStart() {
	auto result = "<!DOCTYPE html>" + _context.pushTag("html");
	result.append(pushTag("head"));
	result.append(pushTag("meta", {
		{ "charset", "utf-8" },
		{ "empty", "" }
	}));
	result.append(pushTag("title", { { "inline", "" } }));
	result.append("Exported Data");
	result.append(popTag());
	result.append(_context.pushTag("meta", {
		{ "name", "viewport" },
		{ "content", "width=device-width, initial-scale=1.0" },
		{ "empty", "" }
	}));
	result.append(_context.pushTag("link", {
		{ "href", _base + "css/style.css" },
		{ "rel", "stylesheet" },
		{ "empty", "" }
	}));
	result.append(popTag());
	result.append(pushTag("body"));
	result.append(pushDiv("page_wrap"));
	return result;
}

HtmlWriter::Wrap::~Wrap() {
	(void)close();
}

HtmlWriter::HtmlWriter() = default;

Result HtmlWriter::start(
		const Settings &settings,
		const Environment &environment,
		Stats *stats) {
	Expects(settings.path.endsWith('/'));

	_settings = base::duplicate(settings);
	_environment = environment;
	_stats = stats;
	_summary = fileWithRelativePath(mainFileRelativePath());

	//const auto result = copyFile(
	//	":/export/css/bootstrap.min.css",
	//	"css/bootstrap.min.css");
	//if (!result) {
	//	return result;
	//}
	const auto copy = [&](const QString &filename) {
		return copyFile(":/export/" + filename, filename);
	};
	const auto files = {
		"css/style.css",
		"images/back.png",
		"images/media_call.png",
		"images/media_contact.png",
		"images/media_file.png",
		"images/media_game.png",
		"images/media_location.png",
		"images/media_music.png",
		"images/media_shop.png",
		"images/media_voice.png",
		"images/section_calls.png",
		"images/section_chats.png",
		"images/section_contacts.png",
		"images/section_frequent.png",
		"images/section_leftchats.png",
		"images/section_other.png",
		"images/section_photos.png",
		"images/section_sessions.png",
		"images/section_web.png",
	};
	for (const auto path : files) {
		const auto name = QString(path);
		if (const auto result = copy(name); !result) {
			return result;
		} else if (const auto png = name.indexOf(".png"); png > 0) {
			const auto x2 = name.mid(0, png) + "@2x.png";
			if (const auto result = copy(x2); !result) {
				return result;
			}
		}
	}
	auto block = _summary->pushHeader("Exported Data");
	block.append(_summary->pushDiv("page_body"));
	return _summary->writeBlock(block);
}

Result HtmlWriter::writePersonal(const Data::PersonalInfo &data) {
	Expects(_summary != nullptr);

	_selfColorIndex = Data::PeerColorIndex(data.user.info.userId);
	if (_settings.types & Settings::Type::Userpics) {
		_delayedPersonalInfo = std::make_unique<Data::PersonalInfo>(data);
		return Result::Success();
	}
	return writeDefaultPersonal(data);
}

Result HtmlWriter::writeDefaultPersonal(const Data::PersonalInfo &data) {
	return writePreparedPersonal(data, QString());
}

Result HtmlWriter::writeDelayedPersonal(const QString &userpicPath) {
	if (!_delayedPersonalInfo) {
		return Result::Success();
	}
	const auto result = writePreparedPersonal(
		*base::take(_delayedPersonalInfo),
		userpicPath);
	if (!result) {
		return result;
	}
	if (_userpicsCount) {
		pushUserpicsSection();
	}
	return Result::Success();
}

Result HtmlWriter::writePreparedPersonal(
		const Data::PersonalInfo &data,
		const QString &userpicPath) {
	const auto &info = data.user.info;

	auto userpic = UserpicData{ _selfColorIndex, kPersonalUserpicSize };
	userpic.largeLink = userpicPath.isEmpty()
		? QString()
		: userpicsFilePath();
	userpic.imageLink = WriteUserpicThumb(
		_settings.path,
		userpicPath,
		userpic,
		"_info");
	userpic.firstName = info.firstName;
	userpic.lastName = info.lastName;

	auto block = _summary->pushDiv("personal_info clearfix");
	block.append(_summary->pushDiv("pull_right userpic_wrap"));
	block.append(_summary->pushUserpic(userpic));
	block.append(_summary->popTag());
	const auto pushRows = [&](
			QByteArray name,
			std::vector<std::pair<QByteArray, QByteArray>> &&values) {
		block.append(_summary->pushDiv("rows " + name));
		for (const auto &[key, value] : values) {
			if (value.isEmpty()) {
				continue;
			}
			block.append(_summary->pushDiv("row"));
			block.append(_summary->pushDiv("label details"));
			block.append(SerializeString(key));
			block.append(_summary->popTag());
			block.append(_summary->pushDiv("value bold"));
			block.append(SerializeString(value));
			block.append(_summary->popTag());
			block.append(_summary->popTag());
		}
		block.append(_summary->popTag());
	};
	pushRows("names", {
		{ "First name", info.firstName },
		{ "Last name", info.lastName },
	});
	pushRows("info", {
		{ "Phone number", Data::FormatPhoneNumber(info.phoneNumber) },
		{ "Username", FormatUsername(data.user.username) },
	});
	pushRows("bio", { { "Bio", data.bio } });
	block.append(_summary->popTag());

	_summaryNeedDivider = true;
	return _summary->writeBlock(block);
}

Result HtmlWriter::writeUserpicsStart(const Data::UserpicsInfo &data) {
	Expects(_summary != nullptr);
	Expects(_userpics == nullptr);

	_userpicsCount = data.count;
	if (!_userpicsCount) {
		return Result::Success();
	}
	_userpics = fileWithRelativePath(userpicsFilePath());

	auto block = _userpics->pushHeader(
		"Personal photos",
		mainFileRelativePath());
	block.append(_userpics->pushDiv("page_body list_page"));
	block.append(_userpics->pushDiv("entry_list"));
	if (const auto result = _userpics->writeBlock(block); !result) {
		return result;
	}
	if (!_delayedPersonalInfo) {
		pushUserpicsSection();
	}
	return Result::Success();
}

Result HtmlWriter::writeUserpicsSlice(const Data::UserpicsSlice &data) {
	Expects(_userpics != nullptr);
	Expects(!data.list.empty());

	const auto firstPath = data.list.front().image.file.relativePath;
	if (const auto result = writeDelayedPersonal(firstPath); !result) {
		return result;
	}

	auto block = QByteArray();
	for (const auto &userpic : data.list) {
		auto data = UserpicData{ _selfColorIndex, kEntryUserpicSize };
		using SkipReason = Data::File::SkipReason;
		const auto &file = userpic.image.file;
		Assert(!file.relativePath.isEmpty()
			|| file.skipReason != SkipReason::None);
		const auto status = [&]() -> Data::Utf8String {
			switch (file.skipReason) {
			case SkipReason::Unavailable:
				return "(Photo unavailable, please try again later)";
			case SkipReason::FileSize:
				return "(Photo exceeds maximum size. "
					"Change data exporting settings to download.)";
			case SkipReason::FileType:
				return "(Photo not included. "
					"Change data exporting settings to download.)";
			case SkipReason::None: return Data::FormatFileSize(file.size);
			}
			Unexpected("Skip reason while writing photo path.");
		}();
		const auto &path = userpic.image.file.relativePath;
		data.imageLink = WriteUserpicThumb(_settings.path, path, data);
		data.firstName = path.toUtf8();
		block.append(_userpics->pushListEntry(
			data,
			(path.isEmpty() ? QString("Photo unavailable") : path).toUtf8(),
			status,
			(userpic.date > 0
				? Data::FormatDateTime(userpic.date)
				: QByteArray()),
			path));
	}
	return _userpics->writeBlock(block);
}

Result HtmlWriter::writeUserpicsEnd() {
	if (const auto result = writeDelayedPersonal(QString()); !result) {
		return result;
	} else if (_userpics) {
		return base::take(_userpics)->close();
	}
	return Result::Success();
}

QString HtmlWriter::userpicsFilePath() const {
	return "lists/profile_pictures.html";
}

void HtmlWriter::pushUserpicsSection() {
	pushSection(
		4,
		"Profile pictures",
		"photos",
		_userpicsCount,
		userpicsFilePath());
}

Result HtmlWriter::writeContactsList(const Data::ContactsList &data) {
	Expects(_summary != nullptr);

	if (const auto result = writeSavedContacts(data); !result) {
		return result;
	} else if (const auto result = writeFrequentContacts(data); !result) {
		return result;
	}
	return Result::Success();
}

Result HtmlWriter::writeSavedContacts(const Data::ContactsList &data) {
	if (data.list.empty()) {
		return Result::Success();
	}

	const auto filename = "lists/contacts.html";
	const auto file = fileWithRelativePath(filename);
	auto block = file->pushHeader(
		"Contacts",
		mainFileRelativePath());
	block.append(file->pushDiv("page_body list_page"));
	block.append(file->pushAbout(_environment.aboutContacts));
	block.append(file->pushDiv("entry_list"));
	for (const auto index : Data::SortedContactsIndices(data)) {
		const auto &contact = data.list[index];
		auto userpic = UserpicData{
			Data::ContactColorIndex(contact),
			kEntryUserpicSize
		};
		userpic.firstName = contact.firstName;
		userpic.lastName = contact.lastName;
		block.append(file->pushListEntry(
			userpic,
			ComposeName(userpic, "Deleted Account"),
			Data::FormatPhoneNumber(contact.phoneNumber),
			Data::FormatDateTime(contact.date)));
	}
	if (const auto result = file->writeBlock(block); !result) {
		return result;
	} else if (const auto closed = file->close(); !closed) {
		return closed;
	}

	pushSection(
		2,
		"Contacts",
		"contacts",
		data.list.size(),
		filename);
	return Result::Success();
}

Result HtmlWriter::writeFrequentContacts(const Data::ContactsList &data) {
	const auto size = data.correspondents.size()
		+ data.inlineBots.size()
		+ data.phoneCalls.size();
	if (!size) {
		return Result::Success();
	}

	const auto filename = "lists/frequent.html";
	const auto file = fileWithRelativePath(filename);
	auto block = file->pushHeader(
		"Frequent contacts",
		mainFileRelativePath());
	block.append(file->pushDiv("page_body list_page"));
	block.append(file->pushAbout(_environment.aboutFrequent));
	block.append(file->pushDiv("entry_list"));
	const auto writeList = [&](
			const std::vector<Data::TopPeer> &peers,
			Data::Utf8String category) {
		for (const auto &top : peers) {
			const auto name = [&]() -> Data::Utf8String {
				if (top.peer.chat()) {
					return top.peer.name();
				} else if (top.peer.user()->isSelf) {
					return "Saved messages";
				} else {
					return top.peer.user()->info.firstName;
				}
			}();
			const auto lastName = [&]() -> Data::Utf8String {
				if (top.peer.user() && !top.peer.user()->isSelf) {
					return top.peer.user()->info.lastName;
				}
				return {};
			}();
			auto userpic = UserpicData{
				Data::PeerColorIndex(Data::BarePeerId(top.peer.id())),
				kEntryUserpicSize
			};
			userpic.firstName = name;
			userpic.lastName = lastName;
			block.append(file->pushListEntry(
				userpic,
				ComposeName(userpic, "Deleted Account"),
				"Rating: " + Data::NumberToString(top.rating),
				category));
		}
	};
	writeList(data.correspondents, "people");
	writeList(data.inlineBots, "inline bots");
	writeList(data.phoneCalls, "calls");
	if (const auto result = file->writeBlock(block); !result) {
		return result;
	} else if (const auto closed = file->close(); !closed) {
		return closed;
	}

	pushSection(
		3,
		"Frequent contacts",
		"frequent",
		size,
		filename);
	return Result::Success();
}

Result HtmlWriter::writeSessionsList(const Data::SessionsList &data) {
	Expects(_summary != nullptr);

	if (const auto result = writeSessions(data); !result) {
		return result;
	} else if (const auto result = writeWebSessions(data); !result) {
		return result;
	}
	return Result::Success();
}

Result HtmlWriter::writeSessions(const Data::SessionsList &data) {
	Expects(_summary != nullptr);

	if (data.list.empty()) {
		return Result::Success();
	}

	const auto filename = "lists/sessions.html";
	const auto file = fileWithRelativePath(filename);
	auto block = file->pushHeader(
		"Sessions",
		mainFileRelativePath());
	block.append(file->pushDiv("page_body list_page"));
	block.append(file->pushAbout(_environment.aboutSessions));
	block.append(file->pushDiv("entry_list"));
	for (const auto &session : data.list) {
		block.append(file->pushSessionListEntry(
			session.applicationId,
			((session.applicationName.isEmpty()
				? Data::Utf8String("Unknown")
				: session.applicationName)
				+ ' '
				+ session.applicationVersion),
			(session.deviceModel
				+ ", "
				+ session.platform
				+ ' '
				+ session.systemVersion),
			{
				(session.ip
					+ " \xE2\x80\x93 "
					+ session.region
					+ ((session.region.isEmpty() || session.country.isEmpty())
						? QByteArray()
						: QByteArray(", "))
					+ session.country),
				"Last active: " + Data::FormatDateTime(session.lastActive),
				"Created: " + Data::FormatDateTime(session.created)
			}));
	}
	if (const auto result = file->writeBlock(block); !result) {
		return result;
	} else if (const auto closed = file->close(); !closed) {
		return closed;
	}

	pushSection(
		5,
		"Sessions",
		"sessions",
		data.list.size(),
		filename);
	return Result::Success();
}

Result HtmlWriter::writeWebSessions(const Data::SessionsList &data) {
	Expects(_summary != nullptr);

	if (data.webList.empty()) {
		return Result::Success();
	}

	const auto filename = "lists/web_sessions.html";
	const auto file = fileWithRelativePath(filename);
	auto block = file->pushHeader(
		"Web sessions",
		mainFileRelativePath());
	block.append(file->pushDiv("page_body list_page"));
	block.append(file->pushAbout(_environment.aboutWebSessions));
	block.append(file->pushDiv("entry_list"));
	for (const auto &session : data.webList) {
		block.append(file->pushSessionListEntry(
			Data::DomainApplicationId(session.domain),
			(session.domain.isEmpty()
				? Data::Utf8String("Unknown")
				: session.domain),
			session.platform + ", " + session.browser,
			{
				session.ip + " \xE2\x80\x93 " + session.region,
				"Last active: " + Data::FormatDateTime(session.lastActive),
				"Created: " + Data::FormatDateTime(session.created)
			},
			(session.botUsername.isEmpty()
				? QByteArray()
				: ('@' + session.botUsername))));
	}
	if (const auto result = file->writeBlock(block); !result) {
		return result;
	} else if (const auto closed = file->close(); !closed) {
		return closed;
	}

	pushSection(
		6,
		"Web sessions",
		"web",
		data.webList.size(),
		filename);
	return Result::Success();
}

Result HtmlWriter::writeOtherData(const Data::File &data) {
	Expects(_summary != nullptr);

	pushSection(
		7,
		"Other data",
		"other",
		1,
		data.relativePath);
	return Result::Success();
}

Result HtmlWriter::writeDialogsStart(const Data::DialogsInfo &data) {
	return writeChatsStart(
		data,
		"Chats",
		"chats",
		_environment.aboutChats,
		"lists/chats.html");
}

Result HtmlWriter::writeDialogStart(const Data::DialogInfo &data) {
	return writeChatStart(data);
}

Result HtmlWriter::writeDialogSlice(const Data::MessagesSlice &data) {
	return writeChatSlice(data);
}

Result HtmlWriter::writeDialogEnd() {
	return writeChatEnd();
}

Result HtmlWriter::writeDialogsEnd() {
	return writeChatsEnd();
}

Result HtmlWriter::writeLeftChannelsStart(const Data::DialogsInfo &data) {
	return writeChatsStart(
		data,
		"Left chats",
		"leftchats",
		_environment.aboutLeftChats,
		"lists/left_chats.html");
}

Result HtmlWriter::writeLeftChannelStart(const Data::DialogInfo &data) {
	return writeChatStart(data);
}

Result HtmlWriter::writeLeftChannelSlice(const Data::MessagesSlice &data) {
	return writeChatSlice(data);
}

Result HtmlWriter::writeLeftChannelEnd() {
	return writeChatEnd();
}

Result HtmlWriter::writeLeftChannelsEnd() {
	return writeChatsEnd();
}

Result HtmlWriter::writeChatsStart(
		const Data::DialogsInfo &data,
		const QByteArray &listName,
		const QByteArray &buttonClass,
		const QByteArray &about,
		const QString &fileName) {
	Expects(_summary != nullptr);
	Expects(_chats == nullptr);

	if (data.list.empty()) {
		return Result::Success();
	}

	_dialogsRelativePath = fileName;
	_chats = fileWithRelativePath(fileName);
	_dialogIndex = 0;
	_dialogsCount = data.list.size();

	auto block = _chats->pushHeader(
		listName,
		mainFileRelativePath());
	block.append(_chats->pushDiv("page_body list_page"));
	block.append(_chats->pushAbout(about));
	block.append(_chats->pushDiv("entry_list"));
	if (const auto result = _chats->writeBlock(block); !result) {
		return result;
	}

	pushSection(
		0,
		listName,
		buttonClass,
		data.list.size(),
		fileName);
	return writeSections();
}

Result HtmlWriter::writeChatStart(const Data::DialogInfo &data) {
	Expects(_chat == nullptr);
	Expects(_dialogIndex < _dialogsCount);

	const auto digits = Data::NumberToString(_dialogsCount - 1).size();
	const auto number = Data::NumberToString(++_dialogIndex, digits, '0');
	_chat = fileWithRelativePath(data.relativePath + messagesFile(0));
	_messagesCount = 0;
	_dateMessageId = 0;
	_lastMessageInfo = nullptr;
	_dialog = data;
	return Result::Success();
}

Result HtmlWriter::writeChatSlice(const Data::MessagesSlice &data) {
	Expects(_chat != nullptr);
	Expects(!data.list.empty());

	if (_chat->empty()) {
		const auto name = (_dialog.name.isEmpty()
			&& _dialog.lastName.isEmpty())
			? QByteArray("Deleted Account")
			: (_dialog.name + ' ' + _dialog.lastName);
		auto block = _chat->pushHeader(
			name,
			_dialogsRelativePath);
		block.append(_chat->pushDiv("page_body chat_page"));
		block.append(_chat->pushDiv("history"));
		if (const auto result = _chat->writeBlock(block); !result) {
			return result;
		}
	}

	const auto wasIndex = (_messagesCount / kMessagesInFile);
	_messagesCount += data.list.size();
	const auto nowIndex = (_messagesCount / kMessagesInFile);
	if (nowIndex != wasIndex) {
		if (const auto result = switchToNextChatFile(nowIndex); !result) {
			return result;
		}
	}

	auto previous = _lastMessageInfo.get();
	auto saved = MessageInfo();
	auto block = QByteArray();
	for (const auto &message : data.list) {
		const auto date = message.date;
		if (DisplayDate(date, previous ? previous->date : 0)) {
			block.append(_chat->pushServiceMessage(
				--_dateMessageId,
				_dialog,
				_settings.path,
				FormatDateText(date)));
		}
		const auto [info, content] = _chat->pushMessage(
			message,
			previous,
			_dialog,
			_settings.path,
			data.peers,
			_environment.internalLinksDomain);
		block.append(content);
		saved = info;
		previous = &saved;
	}
	_lastMessageInfo = std::make_unique<MessageInfo>(saved);
	return _chat->writeBlock(block);
}

Result HtmlWriter::writeChatEnd() {
	Expects(_chats != nullptr);
	Expects(_chat != nullptr);

	if (const auto closed = base::take(_chat)->close(); !closed) {
		return closed;
	}

	using Type = Data::DialogInfo::Type;
	const auto TypeString = [](Type type) {
		switch (type) {
		case Type::Unknown: return "unknown";
		case Type::Self:
		case Type::Personal: return "private";
		case Type::Bot: return "bot";
		case Type::PrivateGroup:
		case Type::PrivateSupergroup:
		case Type::PublicSupergroup: return "group";
		case Type::PrivateChannel:
		case Type::PublicChannel: return "channel";
		}
		Unexpected("Dialog type in TypeString.");
	};
	const auto DeletedString = [](Type type) {
		switch (type) {
		case Type::Unknown:
		case Type::Self:
		case Type::Personal:
		case Type::Bot: return "Deleted Account";
		case Type::PrivateGroup:
		case Type::PrivateSupergroup:
		case Type::PublicSupergroup: return "Deleted Group";
		case Type::PrivateChannel:
		case Type::PublicChannel: return "Deleted Channel";
		}
		Unexpected("Dialog type in TypeString.");
	};
	const auto NameString = [](
			const Data::DialogInfo &dialog) -> QByteArray {
		if (dialog.type == Type::Self) {
			return "Saved messages";
		}
		return dialog.name;
	};
	const auto LastNameString = [](
			const Data::DialogInfo &dialog) -> QByteArray {
		if (dialog.type != Type::Personal && dialog.type != Type::Bot) {
			return {};
		}
		return dialog.lastName;
	};
	const auto CountString = [](int count, bool outgoing) -> QByteArray {
		if (count == 1) {
			return outgoing ? "1 outgoing message" : "1 message";
		} else if (!count) {
			return outgoing ? "No outgoing messages" : "No messages";
		}
		return Data::NumberToString(count)
			+ (outgoing ? " outgoing messages" : " messages");
	};
	auto userpic = UserpicData{
		(_dialog.type == Type::Self
			? kSavedMessagesColorIndex
			: Data::PeerColorIndex(Data::BarePeerId(_dialog.peerId))),
		kEntryUserpicSize
	};
	userpic.firstName = NameString(_dialog);
	userpic.lastName = LastNameString(_dialog);
	return _chats->writeBlock(_chats->pushListEntry(
		userpic,
		ComposeName(userpic, DeletedString(_dialog.type)),
		CountString(_messagesCount, _dialog.onlyMyMessages),
		TypeString(_dialog.type),
		(_messagesCount > 0
			? (_dialog.relativePath + "messages.html")
			: QString())));
}

Result HtmlWriter::writeChatsEnd() {
	if (_chats) {
		return base::take(_chats)->close();
	}
	return Result::Success();
}

void HtmlWriter::pushSection(
		int priority,
		const QByteArray &label,
		const QByteArray &type,
		int count,
		const QString &path) {
	_savedSections.push_back({
		priority,
		label,
		type,
		count,
		path
	});
}

Result HtmlWriter::writeSections() {
	Expects(_summary != nullptr);

	if (!_haveSections && _summaryNeedDivider) {
		auto block = _summary->pushDiv(
			_summaryNeedDivider ? "sections with_divider" : "sections");
		if (const auto result = _summary->writeBlock(block); !result) {
			return result;
		}
		_haveSections = true;
		_summaryNeedDivider = false;
	}

	auto block = QByteArray();
	ranges::sort(_savedSections, std::less<>(), [](const SavedSection &data) {
		return data.priority;
	});
	for (const auto &section : base::take(_savedSections)) {
		block.append(_summary->pushSection(
			section.label,
			section.type,
			section.count,
			_summary->relativePath(section.path)));
	}
	return _summary->writeBlock(block);
}

Result HtmlWriter::switchToNextChatFile(int index) {
	Expects(_chat != nullptr);

	const auto nextPath = messagesFile(index);
	auto next = _chat->pushTag("a", {
		{ "class", "pagination" },
		{ "href", nextPath.toUtf8() }
	});
	next.append("Next messages part");
	next.append(_chat->popTag());
	if (const auto result = _chat->writeBlock(next); !result) {
		return result;
	}
	_chat = fileWithRelativePath(_dialog.relativePath + nextPath);
	auto block = _chat->pushHeader(
		_dialog.name + ' ' + _dialog.lastName,
		_dialogsRelativePath);
	block.append(_chat->pushDiv("page_body chat_page"));
	block.append(_chat->pushDiv("history"));
	block.append(_chat->pushTag("a", {
		{ "class", "pagination" },
		{ "href", nextPath.toUtf8() }
	}));
	block.append("Previous messages part");
	block.append(_chat->popTag());
	return _chat->writeBlock(block);
}

Result HtmlWriter::finish() {
	Expects(_summary != nullptr);

	auto block = QByteArray();
	if (_haveSections) {
		block.append(_summary->popTag());
		_summaryNeedDivider = true;
		_haveSections = false;
	}
	block.append(_summary->pushAbout(
		_environment.aboutTelegram,
		_summaryNeedDivider));
	if (const auto result = _summary->writeBlock(block); !result) {
		return result;
	}
	return _summary->close();
}

Result HtmlWriter::copyFile(
		const QString &source,
		const QString &relativePath) const {
	return File::Copy(
		source,
		pathWithRelativePath(relativePath),
		_stats);
}

QString HtmlWriter::mainFilePath() {
	return pathWithRelativePath(mainFileRelativePath());
}

QString HtmlWriter::mainFileRelativePath() const {
	return "export_results.html";
}

QString HtmlWriter::pathWithRelativePath(const QString &path) const {
	return _settings.path + path;
}

QString HtmlWriter::messagesFile(int index) const {
	return "messages"
		+ (index > 0 ? QString::number(index + 1) : QString())
		+ ".html";
}

std::unique_ptr<HtmlWriter::Wrap> HtmlWriter::fileWithRelativePath(
		const QString &path) const {
	return std::make_unique<Wrap>(
		pathWithRelativePath(path),
		_settings.path,
		_stats);
}

HtmlWriter::~HtmlWriter() = default;

} // namespace Output
} // namespace Export
