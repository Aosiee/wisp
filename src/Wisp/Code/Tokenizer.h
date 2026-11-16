#pragma once

// Syntax colouring for wisp::CodeEditor. A tokenizer works a line at a time
// and carries a 32-bit state across lines (an open long string or block
// comment), so the editor re-tokenizes from the first changed line only.
// LuauTokenizer is the one implementation: pure Luau syntax plus a
// caller-supplied set of names to show as globals (the editor passes the
// engine's script API).

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace wisp::code {

	enum class TokenKind : uint8_t
	{
		Plain,
		Keyword,
		Literal,  // true / false / nil / self
		Number,
		String,
		Comment,
		Operator,
		Function, // a name followed by '('
		Global,   // a caller-listed name (the engine's API, the standard library)
	};

	struct Token
	{
		int Start = 0;  // byte column
		int Length = 0;
		TokenKind Kind = TokenKind::Plain;
	};

	class ITokenizer
	{
	public:
		virtual ~ITokenizer() = default;
		// Appends `line`'s tokens to `out` (gaps are plain); returns the
		// state the next line starts in (0 = nothing open).
		virtual uint32_t TokenizeLine(std::string_view line, uint32_t state, std::vector<Token>& out) const = 0;
	};

	// Luau, or cluau (`//` and `/* */` comments, `--` an operator,
	// `let` / `const` / `class` keywords, `@attribute`s).
	enum class LuauSyntax : uint8_t { Luau, Cluau };

	class LuauTokenizer final : public ITokenizer
	{
	public:
		explicit LuauTokenizer(std::unordered_set<std::string> globals = {}, LuauSyntax syntax = LuauSyntax::Luau);
		uint32_t TokenizeLine(std::string_view line, uint32_t state, std::vector<Token>& out) const override;

	private:
		std::unordered_set<std::string> m_Globals;
		LuauSyntax m_Syntax = LuauSyntax::Luau;
	};

} // namespace wisp::code
