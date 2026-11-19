// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/html_filter.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

#include "base/json/json_reader.h"
#include "base/memory/ptr_util.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "starry/components/content_filter/css_selector.h"
#include "starry/components/content_filter/html_document.h"
#include "starry/components/content_filter/js_regex.h"
#include "third_party/re2/src/re2/re2.h"
#include "url/gurl.h"

namespace starry::content_filter {

namespace {

using NodeId = HtmlDocument::NodeId;
constexpr NodeId kNone = HtmlDocument::kNone;
constexpr size_t kNpos = std::string_view::npos;
// :has()、:not() 最多套几层。
constexpr int kMaxDepth = 8;

// UTF-8 的文字在 JS 里有多长（UTF-16 的单元数，同 textContent.length）。
size_t Utf16Length(std::string_view text) {
  size_t length = 0;
  for (char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if ((u & 0xC0) != 0x80) {
      length += u >= 0xF0 ? 2 : 1;
    }
  }
  return length;
}

// 文字或名字的匹配：编出的 JSON 是 `"正则","标志"`（字面的文字已经转成了正则）。
class Matcher {
 public:
  bool Compile(const std::string& source, const std::string& flags) {
    if (flags.empty() && TakeLiteral(source)) {
      return true;
    }
    if (TakeMinLength(source)) {
      return true;
    }
    std::string pattern;
    if (!TranslateJsRegexToRe2(source, &pattern)) {
      return false;
    }
    std::string prefix;
    if (flags.find('m') != std::string::npos) {
      prefix += "(?m)";
    }
    re2::RE2::Options options;
    options.set_log_errors(false);
    options.set_case_sensitive(flags.find('i') == std::string::npos);
    options.set_dot_nl(flags.find('s') != std::string::npos);
    options.set_max_mem(2 << 20);
    regex_ = std::make_unique<re2::RE2>(prefix + pattern, options);
    return regex_->ok();
  }

  bool Matches(std::string_view text) const {
    if (min_length_) {
      return Utf16Length(text) >= *min_length_;
    }
    if (!regex_) {
      return text.find(literal_) != kNpos;
    }
    return re2::RE2::PartialMatch(text, *regex_);
  }

 private:
  // `[\w\W]{n}`、`[\s\S]{n,}`：至少 n 个字（列表里用来找很长的脚本，n 常常超过 RE2 的重复
  // 上限 1000）。
  bool TakeMinLength(std::string_view source) {
    bool any = false;
    for (std::string_view prefix : {"[\\w\\W]{", "[\\W\\w]{", "[\\s\\S]{",
                                    "[\\S\\s]{", "[\\d\\D]{", "[\\D\\d]{",
                                    "[^]{"}) {
      if (source.starts_with(prefix)) {
        source.remove_prefix(prefix.size());
        any = true;
        break;
      }
    }
    if (!any || !source.ends_with('}')) {
      return false;
    }
    source.remove_suffix(1);
    const size_t comma = source.find(',');
    const std::string_view min = source.substr(0, comma);
    const std::string_view max =
        comma == kNpos ? std::string_view() : source.substr(comma + 1);
    size_t n = 0;
    if (min.empty() || !base::StringToSizeT(min, &n) ||
        (!max.empty() && !std::all_of(max.begin(), max.end(),
                                      base::IsAsciiDigit<char>))) {
      return false;
    }
    min_length_ = n;
    return true;
  }

  // 只有转义了的元字符、没有别的正则写法的（`:has-text(文字)`）：直接找字符串。
  bool TakeLiteral(std::string_view source) {
    std::string literal;
    for (size_t i = 0; i < source.size(); ++i) {
      const char c = source[i];
      if (c == '\\') {
        if (i + 1 >= source.size() ||
            std::string_view("\\^$.*+?()[]{}|/").find(source[i + 1]) ==
                kNpos) {
          return false;
        }
        literal += source[++i];
      } else if (std::string_view("^$.*+?()[]{}|").find(c) != kNpos) {
        return false;
      } else {
        literal += c;
      }
    }
    if (literal.empty()) {
      return false;
    }
    literal_ = std::move(literal);
    return true;
  }

  std::string literal_;
  std::unique_ptr<re2::RE2> regex_;
  std::optional<size_t> min_length_;
};

bool IsCombinator(char c) {
  return c == '>' || c == '+' || c == '~';
}

void SortUnique(std::vector<NodeId>* nodes) {
  std::sort(nodes->begin(), nodes->end());
  nodes->erase(std::unique(nodes->begin(), nodes->end()), nodes->end());
}

}  // namespace

namespace internal {

// 同 procedural.js 的 compileSelector：开头的原生选择器，后面一步一步筛、换。
struct HtmlProgram {
  struct Task {
    enum class Kind : uint8_t {
      kSpath,
      kMatch,
      kText,
      kMinLength,
      kHas,
      kNot,
      kUp,
      kUpSelector,
      kAttribute,
      kPath,
    };

    Kind kind = Kind::kMatch;
    std::unique_ptr<CssSelector> selector;
    Matcher matcher;
    std::optional<Matcher> value_matcher;
    std::unique_ptr<HtmlProgram> program;
    int count = 0;
  };

  // 空的时从输入（文档、:has() 外面的元素）开始。
  std::unique_ptr<CssSelector> base;
  std::vector<Task> tasks;
};

}  // namespace internal

namespace {

using internal::HtmlProgram;
using Task = HtmlProgram::Task;

std::unique_ptr<HtmlProgram> CompileProgram(const base::Value& value,
                                            bool nested,
                                            int depth) {
  if (depth > kMaxDepth || !value.is_list()) {
    return nullptr;
  }
  const base::ListValue& spec = value.GetList();
  if (spec.size() != 2 || !spec[0].is_string() || !spec[1].is_list()) {
    return nullptr;
  }
  auto program = std::make_unique<HtmlProgram>();
  const std::string_view base = base::TrimWhitespaceASCII(
      spec[0].GetString(), base::TRIM_ALL);
  if (!base.empty()) {
    // `> x`、`+ x`：只在 :has()、:not() 里，相对于外面的元素。
    program->base =
        CssSelector::Parse(base, nested && IsCombinator(base.front()));
    if (!program->base) {
      return nullptr;
    }
  }
  for (const base::Value& item : spec[1].GetList()) {
    if (!item.is_list() || item.GetList().empty() ||
        !item.GetList()[0].is_string()) {
      return nullptr;
    }
    const base::ListValue& args = item.GetList();
    const std::string& op = args[0].GetString();
    auto string_arg = [&](size_t i) -> const std::string* {
      return i < args.size() && args[i].is_string() ? &args[i].GetString()
                                                    : nullptr;
    };
    auto matcher = [&](size_t i, Matcher* out) {
      const std::string* source = string_arg(i);
      const std::string* flags = string_arg(i + 1);
      return source && flags && out->Compile(*source, *flags);
    };
    Task task;
    bool ok = false;
    if (op == "spath" || op == "match" || op == "upsel") {
      task.kind = op == "spath"   ? Task::Kind::kSpath
                  : op == "match" ? Task::Kind::kMatch
                                  : Task::Kind::kUpSelector;
      if (const std::string* selector = string_arg(1)) {
        task.selector = CssSelector::Parse(*selector, op == "spath");
        ok = !!task.selector;
      }
    } else if (op == "text" || op == "path") {
      task.kind = op == "text" ? Task::Kind::kText : Task::Kind::kPath;
      ok = matcher(1, &task.matcher);
    } else if (op == "minlen" || op == "up") {
      task.kind = op == "up" ? Task::Kind::kUp : Task::Kind::kMinLength;
      if (args.size() == 2 && args[1].is_int() && args[1].GetInt() >= 0) {
        task.count = args[1].GetInt();
        ok = true;
      }
    } else if (op == "has" || op == "not") {
      task.kind = op == "has" ? Task::Kind::kHas : Task::Kind::kNot;
      if (args.size() == 2) {
        task.program = CompileProgram(args[1], /*nested=*/true, depth + 1);
        ok = !!task.program;
      }
    } else if (op == "attr") {
      task.kind = Task::Kind::kAttribute;
      ok = matcher(1, &task.matcher);
      if (ok && args.size() > 3) {
        task.value_matcher.emplace();
        ok = matcher(3, &*task.value_matcher);
      }
    } else if (op == "watch") {
      // 只是让网页里的脚本多看几个属性，源码上不用管。
      continue;
    }
    // 别的（xpath、css、media、others）源码上做不到。
    if (!ok) {
      return nullptr;
    }
    program->tasks.push_back(std::move(task));
  }
  if (!program->base && program->tasks.empty()) {
    return nullptr;
  }
  return program;
}

bool PathMatches(const Matcher& matcher, const GURL& url) {
  const std::string path = url.PathForRequest();
  if (matcher.Matches(path)) {
    return true;
  }
  // 网址里的中文这些是 %XX，规则里可能直接写（同 procedural.js）。
  const std::string decoded = base::UnescapeURLComponent(
      path, base::UnescapeRule::SPACES | base::UnescapeRule::PATH_SEPARATORS |
                base::UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS);
  return decoded != path && matcher.Matches(decoded);
}

std::vector<NodeId> Run(const HtmlProgram& program,
                        const HtmlDocument& doc,
                        NodeId input,
                        const GURL& url);

std::vector<NodeId> RunTask(const Task& task,
                            const HtmlDocument& doc,
                            const std::vector<NodeId>& nodes,
                            const GURL& url) {
  std::vector<NodeId> out;
  std::string storage;
  switch (task.kind) {
    case Task::Kind::kSpath:
      for (NodeId node : nodes) {
        task.selector->Query(doc, node, &out);
      }
      SortUnique(&out);
      break;
    case Task::Kind::kMatch:
      for (NodeId node : nodes) {
        if (task.selector->Matches(doc, node, kNone)) {
          out.push_back(node);
        }
      }
      break;
    case Task::Kind::kText:
      for (NodeId node : nodes) {
        if (task.matcher.Matches(doc.Text(node, &storage))) {
          out.push_back(node);
        }
      }
      break;
    case Task::Kind::kMinLength:
      for (NodeId node : nodes) {
        if (Utf16Length(doc.Text(node, &storage)) >=
            static_cast<size_t>(task.count)) {
          out.push_back(node);
        }
      }
      break;
    case Task::Kind::kHas:
    case Task::Kind::kNot: {
      const bool want = task.kind == Task::Kind::kHas;
      for (NodeId node : nodes) {
        if (Run(*task.program, doc, node, url).empty() != want) {
          out.push_back(node);
        }
      }
      break;
    }
    case Task::Kind::kUp:
      for (NodeId node : nodes) {
        NodeId e = node;
        for (int i = 0; i < task.count && e != kNone; ++i) {
          e = doc.ParentElement(e);
        }
        if (e != kNone) {
          out.push_back(e);
        }
      }
      SortUnique(&out);
      break;
    case Task::Kind::kUpSelector:
      for (NodeId node : nodes) {
        for (NodeId e = doc.ParentElement(node); e != kNone;
             e = doc.ParentElement(e)) {
          if (task.selector->Matches(doc, e, kNone)) {
            out.push_back(e);
            break;
          }
        }
      }
      SortUnique(&out);
      break;
    case Task::Kind::kAttribute:
      for (NodeId node : nodes) {
        if (!doc.IsElement(node)) {
          continue;
        }
        for (const HtmlDocument::Attribute& a : doc.node(node).attributes) {
          if (task.matcher.Matches(a.name) &&
              (!task.value_matcher || task.value_matcher->Matches(a.value))) {
            out.push_back(node);
            break;
          }
        }
      }
      break;
    case Task::Kind::kPath:
      if (PathMatches(task.matcher, url)) {
        out = nodes;
      }
      break;
  }
  return out;
}

std::vector<NodeId> Run(const HtmlProgram& program,
                        const HtmlDocument& doc,
                        NodeId input,
                        const GURL& url) {
  std::vector<NodeId> nodes;
  if (program.base) {
    program.base->Query(doc, input, &nodes);
  } else {
    nodes.push_back(input);
  }
  for (const Task& task : program.tasks) {
    if (nodes.empty()) {
      break;
    }
    nodes = RunTask(task, doc, nodes, url);
  }
  return nodes;
}

// 开头的 `<meta charset=…>`、`<meta http-equiv=Content-Type content="…; charset=…">`。
std::string PrescanMetaCharset(std::string_view head) {
  const std::string lower = base::ToLowerASCII(head);
  for (size_t i = lower.find("<meta"); i != std::string::npos;
       i = lower.find("<meta", i + 5)) {
    const size_t end = lower.find('>', i);
    if (end == std::string::npos) {
      break;
    }
    const std::string_view tag = std::string_view(lower).substr(i, end - i);
    for (size_t c = tag.find("charset"); c != kNpos;
         c = tag.find("charset", c + 7)) {
      size_t j = c + 7;
      while (j < tag.size() && tag[j] == ' ') {
        ++j;
      }
      if (j >= tag.size() || tag[j] != '=') {
        continue;
      }
      ++j;
      while (j < tag.size() && (tag[j] == ' ' || tag[j] == '"' ||
                                tag[j] == '\'')) {
        ++j;
      }
      size_t k = j;
      while (k < tag.size() && std::string_view(" \"';/>").find(tag[k]) ==
                                   kNpos) {
        ++k;
      }
      if (k > j) {
        // 原文的大小写不要紧（NormalizeCharset 会转小写）。
        return std::string(tag.substr(j, k - j));
      }
    }
  }
  return std::string();
}

}  // namespace

HtmlSelector::HtmlSelector(std::unique_ptr<internal::HtmlProgram> program)
    : program_(std::move(program)) {}

HtmlSelector::~HtmlSelector() = default;

// static
std::unique_ptr<HtmlSelector> HtmlSelector::Compile(std::string_view spec) {
  std::optional<base::Value> value =
      base::JSONReader::Read(spec, base::JSON_PARSE_RFC);
  if (!value) {
    return nullptr;
  }
  std::unique_ptr<HtmlProgram> program =
      CompileProgram(*value, /*nested=*/false, 0);
  if (!program) {
    return nullptr;
  }
  return base::WrapUnique(new HtmlSelector(std::move(program)));
}

std::vector<uint32_t> HtmlSelector::Select(const HtmlDocument& doc,
                                           const GURL& url) const {
  std::vector<NodeId> nodes =
      Run(*program_, doc, HtmlDocument::kDocument, url);
  std::erase_if(nodes, [&](NodeId id) { return !doc.IsElement(id); });
  return nodes;
}

bool NormalizeCharset(std::string_view label, std::string* out) {
  std::string name = base::ToLowerASCII(
      base::TrimString(label, " \t\n\r\f\"'", base::TRIM_ALL));
  if (name.empty()) {
    out->clear();
    return true;
  }
  auto any_of = [&](std::initializer_list<std::string_view> names) {
    return std::find(names.begin(), names.end(), name) != names.end();
  };
  if (any_of({"utf-8", "utf8", "unicode-1-1-utf-8", "unicode11utf8",
              "unicode20utf8", "x-unicode20utf8"})) {
    *out = "utf-8";
    return true;
  }
  if (name.starts_with("utf-16") || name.starts_with("utf-32") ||
      name.starts_with("ucs-2") || name.starts_with("iso-2022") ||
      name.starts_with("csiso2022") ||
      any_of({"unicode", "unicodefeff", "unicodefffe", "csunicode",
              "iso-10646-ucs-2", "utf-7", "hz-gb-2312", "replacement"})) {
    return false;
  }
  if (any_of({"gbk", "gb2312", "gb_2312", "gb_2312-80", "x-gbk", "chinese",
              "csgb2312", "csiso58gb231280", "iso-ir-58", "gb18030"})) {
    *out = "gb18030";
  } else if (any_of({"iso-8859-1", "iso8859-1", "iso_8859-1", "latin1", "l1",
                     "us-ascii", "ascii", "ansi_x3.4-1968", "cp1252", "cp819",
                     "ibm819", "iso-ir-100", "csisolatin1", "x-cp1252",
                     "x-user-defined"})) {
    *out = "windows-1252";
  } else if (any_of({"shift_jis", "sjis", "ms_kanji", "x-sjis", "windows-31j",
                     "csshiftjis", "ms932"})) {
    *out = "shift_jis";
  } else if (any_of({"euc-kr", "ks_c_5601-1987", "korean", "windows-949",
                     "cp949", "ksc5601", "ksc_5601", "csksc56011987",
                     "iso-ir-149"})) {
    *out = "windows-949";
  } else if (any_of({"big5", "big5-hkscs", "cn-big5", "csbig5", "x-x-big5"})) {
    *out = "big5";
  } else {
    *out = std::move(name);
  }
  return true;
}

bool DetectHtmlCharset(std::string_view html,
                       std::string_view header_charset,
                       std::string* charset) {
  if (html.starts_with("\xEF\xBB\xBF")) {
    *charset = "utf-8";
    return true;
  }
  if (html.starts_with("\xFE\xFF") || html.starts_with("\xFF\xFE")) {
    return false;
  }
  if (!header_charset.empty()) {
    if (!NormalizeCharset(header_charset, charset)) {
      return false;
    }
  } else {
    const std::string meta = PrescanMetaCharset(html.substr(0, 4096));
    if (!NormalizeCharset(meta, charset)) {
      // `<meta charset=utf-16>` 规范上当作 UTF-8；ISO-2022-JP 这些照用，不过滤。
      if (!meta.starts_with("utf-16")) {
        return false;
      }
      *charset = "utf-8";
    }
  }
  if (charset->empty()) {
    // 没写：是合法的 UTF-8 就是（浏览器会猜，ASCII 的部分不受影响）。
    *charset = base::IsStringUTF8(html) ? "utf-8" : "windows-1252";
  }
  return true;
}

std::optional<std::string> FilterHtml(
    std::string_view html,
    std::string_view header_charset,
    const GURL& url,
    base::span<const HtmlSelector* const> selectors,
    size_t* removed) {
  if (removed) {
    *removed = 0;
  }
  std::string charset;
  if (selectors.empty() || html.empty() ||
      !DetectHtmlCharset(html, header_charset, &charset)) {
    return std::nullopt;
  }
  const HtmlDocument doc(html, charset);
  std::vector<bool> marked(doc.size());
  bool any = false;
  for (const HtmlSelector* selector : selectors) {
    for (NodeId id : selector->Select(doc, url)) {
      if (id != doc.root()) {
        marked[id] = true;
        any = true;
      }
    }
  }
  if (!any) {
    return std::nullopt;
  }
  // 按编号（先序）走：外层删掉了，里面的跳过。
  std::string out;
  out.reserve(html.size());
  size_t copied = 0;
  NodeId skip_until = 0;
  size_t count = 0;
  for (NodeId id = 1; id < doc.size(); ++id) {
    if (id < skip_until || !marked[id]) {
      continue;
    }
    const HtmlDocument::Node& node = doc.node(id);
    skip_until = node.subtree_end;
    if (node.begin < copied || node.end <= node.begin) {
      continue;
    }
    out.append(html.substr(copied, node.begin - copied));
    copied = node.end;
    ++count;
  }
  if (count == 0) {
    return std::nullopt;
  }
  out.append(html.substr(copied));
  if (removed) {
    *removed = count;
  }
  return out;
}

}  // namespace starry::content_filter
