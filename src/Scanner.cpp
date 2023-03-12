#include "Scanner.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <string_view>

#include "ErrorInfo.hpp"

// Size of the buffer for storing an errno corresponding message.
const size_t ERR_MSG_BUFF_SIZE = 256U;

/**
 * Context of an ongoing scan operation. Each scan operation creates an
 * instance of ScanContext at its start. The context stores book-keeping data for the
 * scan process and is passed around (and modified) by the different methods of the
 * Scanner class.
 */
struct ScanContext {
    // The source input being scanned.
    // A string_view here is safe - the lifetime of a ScanContext is limited to
    // a single scan operation. The string_view allows avoiding both copying strings and
    // having to declare a const data member.
    std::string_view srcInput;
    // Use lowercase keyword?
    bool lowerCaseKeywords;
    // Should currColumn be ignored? (Look in currColumn description for more details)
    bool ignoreCurrColumn;
    // Index, in the src input, of the character being scanned.
    int lexPos{0};
    // Number of the line from the src input currently being scanned.
    int currLine{1};
    // Number of the column (of the current line) from the src input currently
    // being scanned. Column information should be ignored if there's at least one '\t' in
    // the source file.
    int currColumn{1};
    // The tokens (and errors) found by the ongoing scan operation.
    ScanResults results;

    ScanContext(const std::string& srcInput, bool lowerKey, bool ignoreCurrColumn)
        : srcInput{srcInput},
          lowerCaseKeywords{lowerKey},
          ignoreCurrColumn{ignoreCurrColumn} {}
};

ScanResults Scanner::scanSrcFile(const std::string& srcFilePath, bool lowerCaseKeywords) {
    std::string src;
    { // Scope for the srcFile ifstream - allows its destruction before lexing
      // work actually takes place.
        std::ifstream srcFile(srcFilePath);
        if (!srcFile.is_open()) {
            // Some error happened during file opening.
            ScanResults res;
            res.errors.emplace_back(
                  ErrorInfo{.msg = std::string{"File '"} + srcFilePath +
                                   "' not found or not available for reading."});
            return res;
        }
        while (srcFile) {
            // Reads the source file line by line and stores its contents in
            // primary memory.
            std::string nextLine;
            std::getline(srcFile, nextLine);
            src.append(nextLine);
            // As std::getline consumes the delimiter - in this case the default
            // "\n" - we add it to the string. We may end up adding an extra
            // "\n" when the last line in the source file didn't end with a "\n"
            // - for the context of the scanner, though, this extra line break
            // is harmless as the new last line in memory will have no code to
            // be scanned.
            src.append("\n");
        }
        srcFile.close();
        if (srcFile.bad()) {
            // Some error happened during the file read operation.
            // NOTE: fail() should not be used for detecting read errors in this
            //       context: a file whose last line consists solely of the
            //       new line character would cause getline to return no
            //       character and set both eofbit and failbit.
            ScanResults res;
            std::array<char, ERR_MSG_BUFF_SIZE> errBuf{};
#if defined(__MSVCRT__) || defined(_MSC_VER)
            // On Windows, using the Microsoft supplied runtime, the safe
            // version of strerror is not strerror_r, but strerror_s.
            strerror_s(errBuf.data(), errBuf.size(), errno);
#else
            strerror_r(errno, errBuf.data(), errBuf.size());
#endif
            res.errors.emplace_back(
                  ErrorInfo{.msg = std::string{"Error while reading '" + srcFilePath +
                                               "': " + errBuf.data()}});
            return res;
        }
    }
    // Scans the source file from its in-memory storage.
    return scan(src, lowerCaseKeywords);
}


ScanResults Scanner::scan(const std::string& src, bool lowerCaseKeywords) {
    // Current column information should be ignored when the source file has at least one tab:
    // The information of how many columns correspond to a '\t' is not in the source file
    // and cannot be easily infered.
    bool srcHasTab = src.find('\t') != std::string::npos;
    ScanContext ctx(src, lowerCaseKeywords, srcHasTab);

    while (!allScanned(ctx)) {
        Scanner::scanNextToken(ctx);
    }

    return ctx.results;
}

bool Scanner::allScanned(const ScanContext& ctx) {
    return ctx.lexPos >= ctx.srcInput.length();
}

char Scanner::nextChr(ScanContext& ctx) {
    char chr = ctx.srcInput[ctx.lexPos];
    ctx.lexPos++;
    return chr;
}

char Scanner::nextChrNoAdvance(const ScanContext& ctx) {
    if (allScanned(ctx)) {
        return '\0';
    }
    return ctx.srcInput[ctx.lexPos];
}

bool Scanner::nextChrMatch(ScanContext& ctx, char expChr) {
    if (allScanned(ctx) || ctx.srcInput[ctx.lexPos] != expChr) {
        return false;
    }
    ctx.lexPos++;
    return true;
}

void Scanner::scanNextToken(ScanContext& ctx) {
    char chr = nextChr(ctx);

    switch (chr) {
        // Handling of single-char tokens
        case '&':
        case ',':
        case '.':
        case '=':
        case '#':
        case '[':
        case '-':
        case '+':
        case ']':
        case ')':
            // A close parenthesis matched in this context won't be ont of the comments
            // terminating characters. Such right parenthesis will be consumed by th comment
            // consuming loop.
        case ';':
        case '*':
            // A star matched in this context won't be one of the comments terminating
            // characters. Such stars will be consumed by the comment consuming loop.
        case '~':
            try {
                ctx.results.tokens.emplace_back(Token{.type = Token::typeFromChar(chr),
                                                      .lexeme = std::string{chr},
                                                      .line = ctx.currLine});
            } catch (std::invalid_argument const& ex) {
                ctx.results.errors.emplace_back(
                      ErrorInfo{.line = ctx.currLine,
                                .column = ctx.ignoreCurrColumn ? -1 : ctx.currColumn,
                                .msg = ex.what()});
            }
            ctx.currColumn++;
            break;

        // Handling of (potentially) two-char tokens
        case '<':
            if (nextChrMatch(ctx, '=')) {
                ctx.results.tokens.emplace_back(Token{
                      .type = TokenType::LESS_EQUAL, .lexeme = "<=", .line = ctx.currLine});
                ctx.currColumn += 2;
            } else {
                ctx.results.tokens.emplace_back(Token{.type = Token::typeFromChar(chr),
                                                      .lexeme = std::string{chr},
                                                      .line = ctx.currLine});
                ctx.currColumn++;
            }
            break;
        case '>':
            if (nextChrMatch(ctx, '=')) {
                ctx.results.tokens.emplace_back(Token{
                      .type = TokenType::GREATER_EQUAL, .lexeme = ">=", .line = ctx.currLine});
                ctx.currColumn += 2;
            } else {
                ctx.results.tokens.emplace_back(Token{.type = Token::typeFromChar(chr),
                                                      .lexeme = std::string{chr},
                                                      .line = ctx.currLine});
                ctx.currColumn++;
            }
            break;
        case ':':
            if (nextChrMatch(ctx, '=')) {
                ctx.results.tokens.emplace_back(
                      Token{.type = TokenType::ASSIGN, .lexeme = ":=", .line = ctx.currLine});
                ctx.currColumn += 2;
            } else {
                ctx.results.tokens.emplace_back(Token{.type = Token::typeFromChar(chr),
                                                      .lexeme = std::string{chr},
                                                      .line = ctx.currLine});
                ctx.currColumn++;
            }
            break;

        // Handling of whitespace characters (except newline) - simply consumed.
        case ' ':
        case '\r':
        case '\t':
            ctx.currColumn++;
            break;

        // Handling of new lines (outside comments; new lines in the middle of comments are
        // handled by the comment handler)
        case '\n':
            ctx.currLine++;
            ctx.currColumn = 1;
            break;

        // Handling of (potential) comments. If the "(" is followed by a "*" and indeed starts a
        // comment, the scanning process will be captured by the comment-consuming loop.
        case '(':
            if (nextChrMatch(ctx, '*')) {
                // Found start of comment - "consume" it.
                consumeComment(ctx);
            } else {
                // Found a single-character open parenthesis token.
                ctx.results.tokens.emplace_back(Token{.type = Token::typeFromChar(chr),
                                                      .lexeme = std::string{chr},
                                                      .line = ctx.currLine});
                ctx.currColumn++;
            }
            break;

        default:

            if (std::isalpha(chr) != 0) {
                scanIdentifier(ctx, chr);
            } else if (std::isdigit(chr) != 0) {
                scanNumber(ctx, chr);
            } else {
                ctx.results.errors.emplace_back(ErrorInfo{
                      .line = ctx.currLine,
                      .column = ctx.ignoreCurrColumn ? -1 : ctx.currColumn,
                      .msg = std::string{"Unexpected character, '"} + chr + "' found."});
                ctx.currColumn++;
            }
    }
}

void Scanner::scanNumber(ScanContext& ctx, char firstDigit) {
    std::string numberLex{firstDigit};
    char nextChr = nextChrNoAdvance(ctx);
    while (isdigit(nextChr) != 0) {
        numberLex.push_back(nextChr);
        ctx.lexPos++;
        ctx.currColumn++;
        nextChr = nextChrNoAdvance(ctx);
    }
    ctx.results.tokens.emplace_back(
          Token{.type = TokenType::NUMBER, .lexeme = numberLex, .line = ctx.currLine});
}

void Scanner::scanIdentifier(ScanContext& ctx, char firstLetter) {
    std::string identLex{firstLetter};
    char nextChr = nextChrNoAdvance(ctx);
    while (isalpha(nextChr) != 0 || isdigit(nextChr) != 0) {
        identLex.push_back(nextChr);
        ctx.lexPos++;
        ctx.currColumn++;
        nextChr = nextChrNoAdvance(ctx);
    }
    const TokenType tkType = tokenTypeFromIdentifierLexeme(ctx, identLex);
    ctx.results.tokens.emplace_back(
          Token{.type = tkType, .lexeme = identLex, .line = ctx.currLine});
}

void Scanner::consumeComment(ScanContext& ctx) {
    bool endOfCommentFound = false;
    while (!allScanned(ctx)) {
        // As comments can be "surrounded" by real code (in Oberon-0, comments are not
        // ended by line breaks), the line and column information must be updated.
        if (nextChrNoAdvance(ctx) == '\n') {
            ctx.currLine++;
            ctx.currColumn = 1;
        } else {
            ctx.currColumn++;
        }
        if (nextChrNoAdvance(ctx) == '*') {
            // There's a chance that the end of comment has been reached;
            // Advances the scan and checks if the next character is ")"
            ctx.lexPos++;
            if (nextChrNoAdvance(ctx) == ')') {
                // The end of the comment has indeed been reached.
                ctx.lexPos++;
                ctx.currColumn++;
                endOfCommentFound = true;
                break; // Break-out of the comment consuming loop
            }
        }
        ctx.lexPos++;
    }
    if (!endOfCommentFound) {
        // If the end of the comment has not been found at this point, it means we
        // have an unfinished comment.
        ctx.results.errors.emplace_back(
              ErrorInfo{.line = ctx.currLine,
                        .column = ctx.ignoreCurrColumn ? -1 : ctx.currColumn,
                        .msg = "Source module ends in an unfinished comment."});
    }
}

TokenType Scanner::tokenTypeFromIdentifierLexeme(const ScanContext& ctx,
                                                 const std::string& idLex) {
    TokenType idTokenType = TokenType::IDENT;
    try {
        if (!ctx.lowerCaseKeywords) {
            // The scanner is supporting the standard casing of Oberon keywords: the lexeme must
            // be provided in all upper case to be recognized as a keyword.
            idTokenType = Token::keywordTypeFromLexeme(idLex);
        } else {
            // The scanner is in all lowercase mode - if the lexeme is all lowercase and matches
            // (in a case-insensitive manner) an Oberon keyword, we return the appropriate
            // keyword token type - otherwise, the lexeme is seen as an identifier.
            std::string upperLex(idLex.size(), ' ');
            bool allLower = true;
            for (std::size_t i = 0; i < idLex.size(); i++) {
                if (idLex[i] != std::tolower(idLex[i])) {
                    allLower = false;
                    break;
                }
                upperLex[i] = static_cast<char>(std::toupper(idLex[i]));
            }
            if (allLower) {
                // The identifier lexeme being is all lower case; there's a chance for it to be
                // a keyword - as the corresponding token method expects all upper case lexemes,
                // we give it upperLex, instead of the original idLex argument.
                idTokenType = Token::keywordTypeFromLexeme(upperLex);
            }
        }
    } catch (std::invalid_argument const& e) {
        // The lexeme not being a keyword is not an issue in the context of this method; we
        // can simply ignore the invalid_argument exception.
    }
    return idTokenType;
}
