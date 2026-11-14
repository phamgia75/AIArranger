// Pa5X "Root tree" container (.STG .KSC .PDC): read-only parser and byte-exact writer. EXPERIMENTAL.
//
// Korg publishes no layout. Everything below is OBSERVED in owner Pa5X KST working copies (research notes:
// pa-studio-research/owner-libraries/pa5x-kst/KST_BINARY_STRUCTURE_FINDINGS.md §2). Only rules that held in every
// observed file are accepted; any other input is refused, never guessed at.
//
//   node   = tag NUL name NUL u32-BE payloadLength payload
//   tag    = "Root" | "Branch" | "Leaf"
//   Root   : the one top-level node, name = the single byte 0x01, file size = 11 + payloadLength
//   Root/Branch payload = child nodes filling payloadLength exactly; Leaf payload = opaque bytes (SMF or XML
//   observed), carried byte-for-byte. No checksum, compression or padding.
//
// The format has no free bytes, so write(parse(f)) == f for every file parse() accepts. The second Pa-native
// container in a KST (KORF: .PCG .PCM .KMP ...) is read by panative_reader.h.
#ifndef AI_ARRANGER_MEDIA_PANATIVE_ROOT_TREE_H
#define AI_ARRANGER_MEDIA_PANATIVE_ROOT_TREE_H

#include "media/panative/panative_reader.h"   // kMaxFileBytes, detail::be32
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ai_arranger::media::panative::roottree {

constexpr int kMaxDepth = 32;   // deepest observed STG nesting is under 10; deeper input is refused, not recursed into
constexpr size_t kMaxNodes = 65536;   // observed maximum 513 per file; bounds memory (~80 B per node) on hostile input

struct Node {
    enum Kind { Root, Branch, Leaf } kind = Leaf;
    std::string name;            // raw bytes (user text, Latin-1/UTF-8 as stored); never contains NUL
    std::string payload;         // Leaf only: the raw leaf bytes
    std::vector<Node> children;  // Root/Branch only
};

struct Tree {
    bool ok = false;
    std::string error;
    Node root;
};

namespace detail {
inline const char* tagOf(Node::Kind k) { return k == Node::Root ? "Root" : k == Node::Branch ? "Branch" : "Leaf"; }

// Parses the nodes in [o, end) of p into out. Every length is checked before use.
inline bool parseNodes(const uint8_t* p, size_t o, size_t end, int depth, std::vector<Node>& out, std::string& err, size_t& nodes) {
    if (depth > kMaxDepth) { err = "nesting deeper than " + std::to_string(kMaxDepth) + " at " + std::to_string(o); return false; }
    while (o < end) {
        const void* t = std::memchr(p + o, 0, end - o);
        if (!t) { err = "node tag not terminated at " + std::to_string(o); return false; }
        const size_t te = size_t(static_cast<const uint8_t*>(t) - p);
        const std::string tag(reinterpret_cast<const char*>(p + o), te - o);
        if (++nodes > kMaxNodes) { err = "more than " + std::to_string(kMaxNodes) + " nodes"; return false; }
        Node n;
        if (tag == "Branch") n.kind = Node::Branch;
        else if (tag == "Leaf") n.kind = Node::Leaf;
        else { err = "unknown node tag at " + std::to_string(o) + (tag == "Root" ? " (Root below the top level)" : ""); return false; }
        const void* m = std::memchr(p + te + 1, 0, end - te - 1);
        if (te + 1 >= end || !m) { err = "node name not terminated at " + std::to_string(o); return false; }
        const size_t ne = size_t(static_cast<const uint8_t*>(m) - p);
        n.name.assign(reinterpret_cast<const char*>(p + te + 1), ne - te - 1);
        if (end - ne - 1 < 4) { err = "node length missing at " + std::to_string(o); return false; }
        const size_t len = panative::detail::be32(p + ne + 1), body = ne + 5;
        if (len > end - body) { err = "node at " + std::to_string(o) + " runs past its parent"; return false; }
        if (n.kind == Node::Leaf) n.payload.assign(reinterpret_cast<const char*>(p + body), len);
        else if (!parseNodes(p, body, body + len, depth + 1, n.children, err, nodes)) return false;
        out.push_back(std::move(n));
        o = body + len;
    }
    return true;
}

// depth/nodes count exactly as parseNodes does (Root's children are depth 1), so write() never emits what parse() refuses.
inline bool writeNode(const Node& n, bool top, std::string& out, std::string& err, int depth, size_t& nodes) {
    // The parser reads every Branch's child list (even an empty one) at depth + 1, so a Branch needs depth + 1 <= kMaxDepth.
    if (!top && (depth > kMaxDepth || (n.kind == Node::Branch && depth + 1 > kMaxDepth))) { err = "nesting deeper than " + std::to_string(kMaxDepth); return false; }
    if (!top && ++nodes > kMaxNodes) { err = "more than " + std::to_string(kMaxNodes) + " nodes"; return false; }
    if (out.size() > kMaxFileBytes) { err = "tree larger than the reader's limit"; return false; }
    if (top != (n.kind == Node::Root)) { err = top ? "top-level node is not Root" : "Root below the top level"; return false; }
    if (top && n.name != "\x01") { err = "Root name is not the byte 0x01"; return false; }
    if (n.name.find('\0') != std::string::npos) { err = "node name contains NUL"; return false; }
    if (n.kind == Node::Leaf && !n.children.empty()) { err = "Leaf with children: " + n.name; return false; }
    if (n.kind != Node::Leaf && !n.payload.empty()) { err = "Root/Branch with leaf bytes: " + n.name; return false; }
    out += tagOf(n.kind); out += '\0'; out += n.name; out += '\0';
    const size_t at = out.size();
    out.append(4, '\0');
    if (n.kind == Node::Leaf) out += n.payload;
    else for (const auto& c : n.children) if (!writeNode(c, false, out, err, depth + 1, nodes)) return false;
    const size_t len = out.size() - at - 4;
    if (len > 0xFFFFFFFFu) { err = "node larger than 4 GiB: " + n.name; return false; }
    for (int i = 0; i < 4; ++i) out[at + i] = char(len >> (8 * (3 - i)));
    return true;
}
}  // namespace detail

// True when the bytes start like a Root tree (used to tell the two KST containers apart).
inline bool looksLikeRootTree(const uint8_t* p, size_t n) { return n >= 5 && std::memcmp(p, "Root\0", 5) == 0; }

// Parses an in-memory file. Never reads outside [p, p+n).
inline Tree parse(const uint8_t* p, size_t n) {
    Tree t;
    if (n > kMaxFileBytes) { t.error = "file larger than the reader's limit"; return t; }
    if (n < 11 || std::memcmp(p, "Root\0\x01\0", 7) != 0) { t.error = "not a Root tree (bytes 0-6 are not \"Root\\0\\x01\\0\")"; return t; }
    if (panative::detail::be32(p + 7) != n - 11) { t.error = "Root length does not match the file size (truncated or trailing bytes)"; return t; }
    t.root.kind = Node::Root;
    t.root.name = "\x01";
    size_t nodes = 0;
    if (!detail::parseNodes(p, 11, n, 1, t.root.children, t.error, nodes)) { t.root.children.clear(); return t; }
    t.ok = true;
    return t;
}

// Serializes a tree. Refuses (returns false, out untouched) any tree parse() could not have produced.
inline bool write(const Node& root, std::string& out, std::string& error) {
    std::string b;
    size_t nodes = 0;
    if (!detail::writeNode(root, true, b, error, 0, nodes)) return false;
    if (b.size() > kMaxFileBytes) { error = "tree larger than the reader's limit"; return false; }
    out = std::move(b);
    return true;
}

// Reads one regular file (symlinks not followed) of at most kMaxFileBytes into bytes. Read-only.
inline bool readBytes(const std::filesystem::path& f, std::string& bytes, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(f, ec))) { error = "not a regular file (symlinks are not followed)"; return false; }
    const auto size = std::filesystem::file_size(f, ec);
    if (ec) { error = "cannot stat: " + ec.message(); return false; }
    if (size > kMaxFileBytes) { error = "file larger than the reader's limit"; return false; }
    std::ifstream in(f, std::ios::binary);
    bytes.assign(size_t(size), '\0');
    if (!in || !in.read(bytes.data(), std::streamsize(bytes.size()))) { error = "cannot read"; return false; }
    return true;
}

inline Tree readFile(const std::filesystem::path& f) {
    std::string b;
    Tree t;
    if (!readBytes(f, b, t.error)) return t;
    return parse(reinterpret_cast<const uint8_t*>(b.data()), b.size());
}

}  // namespace ai_arranger::media::panative::roottree

#endif
