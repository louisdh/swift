//===--- CodeCompletion.cpp - Code completion implementation --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/CodeCompletion.h"
#include "CodeCompletionResultBuilder.h"
#include "ExprContextAnalysis.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Comment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LLVM.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/IDE/CodeCompletionCache.h"
#include "swift/IDE/Utils.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Syntax/SyntaxKind.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Comment.h"
#include "clang/AST/CommentVisitor.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/Module.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include <algorithm>
#include <string>

using namespace swift;
using namespace ide;

using CommandWordsPairs = std::vector<std::pair<StringRef, StringRef>>;

enum CodeCompletionCommandKind {
  none,
  keyword,
  recommended,
  recommendedover,
  mutatingvariant,
  nonmutatingvariant,
};

CodeCompletionCommandKind getCommandKind(StringRef Command) {
#define CHECK_CASE(KIND)                                                      \
  if (Command == #KIND)                                                       \
    return CodeCompletionCommandKind::KIND;
  CHECK_CASE(keyword);
  CHECK_CASE(recommended);
  CHECK_CASE(recommendedover);
  CHECK_CASE(mutatingvariant);
  CHECK_CASE(nonmutatingvariant);
#undef CHECK_CASE
  return CodeCompletionCommandKind::none;
}

StringRef getCommandName(CodeCompletionCommandKind Kind) {
#define CHECK_CASE(KIND)                                                    \
  if (CodeCompletionCommandKind::KIND == Kind) {                            \
    static std::string Name(#KIND);                                         \
    return Name;                                                            \
  }
  CHECK_CASE(keyword)
  CHECK_CASE(recommended)
  CHECK_CASE(recommendedover)
  CHECK_CASE(mutatingvariant);
  CHECK_CASE(nonmutatingvariant);
#undef CHECK_CASE
  llvm_unreachable("Cannot handle this Kind.");
}

bool containsInterestedWords(StringRef Content, StringRef Splitter,
                             bool AllowWhitespace) {
  do {
    Content = Content.split(Splitter).second;
    Content = AllowWhitespace ? Content.trim() : Content;
#define CHECK_CASE(KIND)                                                       \
if (Content.startswith(#KIND))                                                 \
return true;
    CHECK_CASE(keyword)
    CHECK_CASE(recommended)
    CHECK_CASE(recommendedover)
    CHECK_CASE(mutatingvariant);
    CHECK_CASE(nonmutatingvariant);
#undef CHECK_CASE
  } while (!Content.empty());
  return false;
}

void splitTextByComma(StringRef Text, std::vector<StringRef>& Subs) {
  do {
    auto Pair = Text.split(',');
    auto Key = Pair.first.trim();
    if (!Key.empty())
      Subs.push_back(Key);
    Text = Pair.second;
  } while (!Text.empty());
}

namespace clang {
namespace comments {
class WordPairsArrangedViewer {
  ArrayRef<std::pair<StringRef, StringRef>> Content;
  std::vector<StringRef> ViewedText;
  std::vector<StringRef> Words;
  StringRef Key;

  bool isKeyViewed(StringRef K) {
    return std::find(ViewedText.begin(), ViewedText.end(), K) != ViewedText.end();
  }

public:
  WordPairsArrangedViewer(ArrayRef<std::pair<StringRef, StringRef>> Content):
    Content(Content) {}

  bool hasNext() {
    Words.clear();
    bool Found = false;
    for (auto P : Content) {
      if (!Found && !isKeyViewed(P.first)) {
        Key = P.first;
        Found = true;
      }
      if (Found && P.first == Key)
        Words.push_back(P.second);
    }
    return Found;
  }

  std::pair<StringRef, ArrayRef<StringRef>> next() {
    bool HasNext = hasNext();
    (void) HasNext;
    assert(HasNext && "Have no more data.");
    ViewedText.push_back(Key);
    return std::make_pair(Key, llvm::makeArrayRef(Words));
  }
};

class ClangCommentExtractor : public ConstCommentVisitor<ClangCommentExtractor> {
  CommandWordsPairs &Words;
  const CommandTraits &Traits;
  std::vector<const Comment *> Parents;

  void visitChildren(const Comment* C) {
    Parents.push_back(C);
    for (auto It = C->child_begin(); It != C->child_end(); ++ It)
      visit(*It);
    Parents.pop_back();
  }

public:
  ClangCommentExtractor(CommandWordsPairs &Words,
                        const CommandTraits &Traits) : Words(Words),
                                                       Traits(Traits) {}
#define CHILD_VISIT(NAME) \
  void visit##NAME(const NAME *C) {\
    visitChildren(C);\
  }
  CHILD_VISIT(FullComment)
  CHILD_VISIT(ParagraphComment)
#undef CHILD_VISIT

  void visitInlineCommandComment(const InlineCommandComment *C) {
    auto Command = C->getCommandName(Traits);
    auto CommandKind = getCommandKind(Command);
    if (CommandKind == CodeCompletionCommandKind::none)
      return;
    auto &Parent = Parents.back();
    for (auto CIT = std::find(Parent->child_begin(), Parent->child_end(), C) + 1;
         CIT != Parent->child_end(); CIT++) {
      if (auto TC = dyn_cast<TextComment>(*CIT)) {
        auto Text = TC->getText();
        std::vector<StringRef> Subs;
        splitTextByComma(Text, Subs);
        auto Kind = getCommandName(CommandKind);
        for (auto S : Subs)
          Words.push_back(std::make_pair(Kind, S));
      } else
        break;
    }
  }
};

void getClangDocKeyword(ClangImporter &Importer, const Decl *D,
                        CommandWordsPairs &Words) {
  ClangCommentExtractor Extractor(Words, Importer.getClangASTContext().
    getCommentCommandTraits());
  if (auto RC = Importer.getClangASTContext().getRawCommentForAnyRedecl(D)) {
    auto RT = RC->getRawText(Importer.getClangASTContext().getSourceManager());
    if (containsInterestedWords(RT, "@", /*AllowWhitespace*/false)) {
      FullComment* Comment = Importer.getClangASTContext().
        getLocalCommentForDeclUncached(D);
      Extractor.visit(Comment);
    }
  }
}
} // end namespace comments
} // end namespace clang

namespace swift {
namespace markup {
class SwiftDocWordExtractor : public MarkupASTWalker {
  CommandWordsPairs &Pairs;
  CodeCompletionCommandKind Kind;
public:
  SwiftDocWordExtractor(CommandWordsPairs &Pairs) :
    Pairs(Pairs), Kind(CodeCompletionCommandKind::none) {}
  void visitKeywordField(const KeywordField *Field) override {
    Kind = CodeCompletionCommandKind::keyword;
  }
  void visitRecommendedField(const RecommendedField *Field) override {
    Kind = CodeCompletionCommandKind::recommended;
  }
  void visitRecommendedoverField(const RecommendedoverField *Field) override {
    Kind = CodeCompletionCommandKind::recommendedover;
  }
  void visitMutatingvariantField(const MutatingvariantField *Field) override {
    Kind = CodeCompletionCommandKind::mutatingvariant;
  }
  void visitNonmutatingvariantField(const NonmutatingvariantField *Field) override {
    Kind = CodeCompletionCommandKind::nonmutatingvariant;
  }
  void visitText(const Text *Text) override {
    if (Kind == CodeCompletionCommandKind::none)
      return;
    StringRef CommandName = getCommandName(Kind);
    std::vector<StringRef> Subs;
    splitTextByComma(Text->str(), Subs);
    for (auto S : Subs)
      Pairs.push_back(std::make_pair(CommandName, S));
  }
};

void getSwiftDocKeyword(const Decl* D, CommandWordsPairs &Words) {
  auto Interested = false;
  for (auto C : D->getRawComment().Comments) {
    if (containsInterestedWords(C.RawText, "-", /*AllowWhitespace*/true)) {
      Interested = true;
      break;
    }
  }
  if (!Interested)
    return;
  static swift::markup::MarkupContext MC;
  auto DC = getSingleDocComment(MC, D);
  if (!DC.hasValue())
    return;
  SwiftDocWordExtractor Extractor(Words);
  for (auto Part : DC.getValue()->getBodyNodes()) {
    switch (Part->getKind()) {
      case ASTNodeKind::KeywordField:
      case ASTNodeKind::RecommendedField:
      case ASTNodeKind::RecommendedoverField:
      case ASTNodeKind::MutatingvariantField:
      case ASTNodeKind::NonmutatingvariantField:
        Extractor.walk(Part);
        break;
      default:
        break;
    }
  }
}
} // end namespace markup
} // end namespace swift

static bool shouldHideDeclFromCompletionResults(const ValueDecl *D) {
  // Hide private stdlib declarations.
  if (D->isPrivateStdlibDecl(/*treatNonBuiltinProtocolsAsPublic*/false) ||
      // ShowInInterfaceAttr is for decls to show in interface as exception but
      // they are not intended to be used directly.
      D->getAttrs().hasAttribute<ShowInInterfaceAttr>())
    return true;

  if (AvailableAttr::isUnavailable(D))
    return true;

  if (auto *ClangD = D->getClangDecl()) {
    if (ClangD->hasAttr<clang::SwiftPrivateAttr>())
      return true;
  }

  // Hide editor placeholders.
  if (D->getBaseName().isEditorPlaceholder())
    return true;

  if (!D->isUserAccessible())
    return true;

  return false;
}

using DeclFilter = std::function<bool(ValueDecl *, DeclVisibilityKind)>;
static bool DefaultFilter(ValueDecl* VD, DeclVisibilityKind Kind) {
  return true;
}
static bool KeyPathFilter(ValueDecl* decl, DeclVisibilityKind) {
  return isa<TypeDecl>(decl) ||
         (isa<VarDecl>(decl) && decl->getDeclContext()->isTypeContext());
}

static bool SwiftKeyPathFilter(ValueDecl* decl, DeclVisibilityKind) {
  switch(decl->getKind()){
  case DeclKind::Var:
  case DeclKind::Subscript:
    return true;
  default:
    return false;
  }
}

std::string swift::ide::removeCodeCompletionTokens(
    StringRef Input, StringRef TokenName, unsigned *CompletionOffset) {
  assert(TokenName.size() >= 1);

  *CompletionOffset = ~0U;

  std::string CleanFile;
  CleanFile.reserve(Input.size());
  const std::string Token = std::string("#^") + TokenName.str() + "^#";

  for (const char *Ptr = Input.begin(), *End = Input.end();
       Ptr != End; ++Ptr) {
    const char C = *Ptr;
    if (C == '#' && Ptr <= End - Token.size() &&
        StringRef(Ptr, Token.size()) == Token) {
      Ptr += Token.size() - 1;
      *CompletionOffset = CleanFile.size();
      CleanFile += '\0';
      continue;
    }
    if (C == '#' && Ptr <= End - 2 && Ptr[1] == '^') {
      do {
        Ptr++;
      } while (Ptr < End && *Ptr != '#');
      if (Ptr == End)
        break;
      continue;
    }
    CleanFile += C;
  }
  return CleanFile;
}

namespace {
class StmtFinder : public ASTWalker {
  SourceManager &SM;
  SourceLoc Loc;
  StmtKind Kind;
  Stmt *Found = nullptr;

public:
  StmtFinder(SourceManager &SM, SourceLoc Loc, StmtKind Kind)
      : SM(SM), Loc(Loc), Kind(Kind) {}

  std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
    return { SM.rangeContainsTokenLoc(S->getSourceRange(), Loc), S };
  }

  Stmt *walkToStmtPost(Stmt *S) override {
    if (S->getKind() == Kind) {
      Found = S;
      return nullptr;
    }
    return S;
  }

  Stmt *getFoundStmt() const {
    return Found;
  }
};
} // end anonymous namespace

static Stmt *findNearestStmt(const DeclContext *DC, SourceLoc Loc,
                             StmtKind Kind) {
  auto &SM = DC->getASTContext().SourceMgr;
  StmtFinder Finder(SM, Loc, Kind);
  // FIXME(thread-safety): the walker is mutating the AST.
  const_cast<DeclContext *>(DC)->walkContext(Finder);
  return Finder.getFoundStmt();
}
/// Prepare the given expression for type-checking again, prinicipally by
/// erasing any ErrorType types on the given expression, allowing later
/// type-checking to make progress.
///
/// FIXME: this is fundamentally a workaround for the fact that we may end up
/// typechecking parts of an expression more than once - first for checking
/// the context, and later for checking more-specific things like unresolved
/// members.  We should restructure code-completion type-checking so that we
/// never typecheck more than once (or find a more principled way to do it).
static void prepareForRetypechecking(Expr *E) {
  assert(E);
  struct Eraser : public ASTWalker {
    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
      if (expr && expr->getType() && (expr->getType()->hasError() ||
                                      expr->getType()->hasUnresolvedType()))
        expr->setType(Type());
      if (auto *ACE = dyn_cast_or_null<AutoClosureExpr>(expr)) {
        return { true, ACE->getSingleExpressionBody() };
      }
      return { true, expr };
    }
    bool walkToTypeLocPre(TypeLoc &TL) override {
      if (TL.getType() && (TL.getType()->hasError() ||
                           TL.getType()->hasUnresolvedType()))
        TL.setType(Type());
      return true;
    }

    std::pair<bool, Pattern*> walkToPatternPre(Pattern *P) override {
      if (P && P->hasType() && (P->getType()->hasError() ||
                                P->getType()->hasUnresolvedType())) {
        P->setType(Type());
      }
      return { true, P };
    }
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
      return { false, S };
    }
  };

  E->walk(Eraser());
}

CodeCompletionString::CodeCompletionString(ArrayRef<Chunk> Chunks) {
  std::uninitialized_copy(Chunks.begin(), Chunks.end(),
                          getTrailingObjects<Chunk>());
  NumChunks = Chunks.size();
}

CodeCompletionString *CodeCompletionString::create(llvm::BumpPtrAllocator &Allocator,
                                                   ArrayRef<Chunk> Chunks) {
  void *CCSMem = Allocator.Allocate(totalSizeToAlloc<Chunk>(Chunks.size()),
                                    alignof(CodeCompletionString));
  return new (CCSMem) CodeCompletionString(Chunks);
}

void CodeCompletionString::print(raw_ostream &OS) const {
  unsigned PrevNestingLevel = 0;
  for (auto C : getChunks()) {
    bool AnnotatedTextChunk = false;
    if (C.getNestingLevel() < PrevNestingLevel) {
      OS << "#}";
    }
    switch (C.getKind()) {
    using ChunkKind = Chunk::ChunkKind;
    case ChunkKind::AccessControlKeyword:
    case ChunkKind::DeclAttrKeyword:
    case ChunkKind::DeclAttrParamKeyword:
    case ChunkKind::OverrideKeyword:
    case ChunkKind::ThrowsKeyword:
    case ChunkKind::RethrowsKeyword:
    case ChunkKind::DeclIntroducer:
    case ChunkKind::Text:
    case ChunkKind::LeftParen:
    case ChunkKind::RightParen:
    case ChunkKind::LeftBracket:
    case ChunkKind::RightBracket:
    case ChunkKind::LeftAngle:
    case ChunkKind::RightAngle:
    case ChunkKind::Dot:
    case ChunkKind::Ellipsis:
    case ChunkKind::Comma:
    case ChunkKind::ExclamationMark:
    case ChunkKind::QuestionMark:
    case ChunkKind::Ampersand:
    case ChunkKind::Equal:
    case ChunkKind::Whitespace:
      AnnotatedTextChunk = C.isAnnotation();
      LLVM_FALLTHROUGH;
    case ChunkKind::CallParameterName:
    case ChunkKind::CallParameterInternalName:
    case ChunkKind::CallParameterColon:
    case ChunkKind::DeclAttrParamColon:
    case ChunkKind::CallParameterType:
    case ChunkKind::CallParameterClosureType:
    case ChunkKind::GenericParameterName:
      if (AnnotatedTextChunk)
        OS << "['";
      else if (C.getKind() == ChunkKind::CallParameterInternalName)
        OS << "(";
      else if (C.getKind() == ChunkKind::CallParameterClosureType)
        OS << "##";
      for (char Ch : C.getText()) {
        if (Ch == '\n')
          OS << "\\n";
        else
          OS << Ch;
      }
      if (AnnotatedTextChunk)
        OS << "']";
      else if (C.getKind() == ChunkKind::CallParameterInternalName)
        OS << ")";
      break;
    case ChunkKind::OptionalBegin:
    case ChunkKind::CallParameterBegin:
    case ChunkKind::GenericParameterBegin:
      OS << "{#";
      break;
    case ChunkKind::DynamicLookupMethodCallTail:
    case ChunkKind::OptionalMethodCallTail:
      OS << C.getText();
      break;
    case ChunkKind::TypeAnnotation:
      OS << "[#";
      OS << C.getText();
      OS << "#]";
      break;
    case ChunkKind::BraceStmtWithCursor:
      OS << " {|}";
      break;
    }
    PrevNestingLevel = C.getNestingLevel();
  }
  while (PrevNestingLevel > 0) {
    OS << "#}";
    PrevNestingLevel--;
  }
}

void CodeCompletionString::dump() const {
  print(llvm::errs());
}

CodeCompletionDeclKind
CodeCompletionResult::getCodeCompletionDeclKind(const Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::IfConfig:
  case DeclKind::PoundDiagnostic:
  case DeclKind::MissingMember:
    llvm_unreachable("not expecting such a declaration result");
  case DeclKind::Module:
    return CodeCompletionDeclKind::Module;
  case DeclKind::TypeAlias:
    return CodeCompletionDeclKind::TypeAlias;
  case DeclKind::AssociatedType:
    return CodeCompletionDeclKind::AssociatedType;
  case DeclKind::GenericTypeParam:
    return CodeCompletionDeclKind::GenericTypeParam;
  case DeclKind::Enum:
    return CodeCompletionDeclKind::Enum;
  case DeclKind::Struct:
    return CodeCompletionDeclKind::Struct;
  case DeclKind::Class:
    return CodeCompletionDeclKind::Class;
  case DeclKind::Protocol:
    return CodeCompletionDeclKind::Protocol;
  case DeclKind::Var:
  case DeclKind::Param: {
    auto DC = D->getDeclContext();
    if (DC->isTypeContext()) {
      if (cast<VarDecl>(D)->isStatic())
        return CodeCompletionDeclKind::StaticVar;
      else
        return CodeCompletionDeclKind::InstanceVar;
    }
    if (DC->isLocalContext())
      return CodeCompletionDeclKind::LocalVar;
    return CodeCompletionDeclKind::GlobalVar;
  }
  case DeclKind::Constructor:
    return CodeCompletionDeclKind::Constructor;
  case DeclKind::Destructor:
    return CodeCompletionDeclKind::Destructor;
  case DeclKind::Accessor:
  case DeclKind::Func: {
    auto DC = D->getDeclContext();
    auto FD = cast<FuncDecl>(D);
    if (DC->isTypeContext()) {
      if (FD->isStatic())
        return CodeCompletionDeclKind::StaticMethod;
      return CodeCompletionDeclKind::InstanceMethod;
    }
    if (FD->isOperator()) {
      if (auto op = FD->getOperatorDecl()) {
        switch (op->getKind()) {
        case DeclKind::PrefixOperator:
          return CodeCompletionDeclKind::PrefixOperatorFunction;
        case DeclKind::PostfixOperator:
          return CodeCompletionDeclKind::PostfixOperatorFunction;
        case DeclKind::InfixOperator:
          return CodeCompletionDeclKind::InfixOperatorFunction;
        default:
          llvm_unreachable("unexpected operator kind");
        }
      } else {
        return CodeCompletionDeclKind::InfixOperatorFunction;
      }
    }
    return CodeCompletionDeclKind::FreeFunction;
  }
  case DeclKind::InfixOperator:
    return CodeCompletionDeclKind::InfixOperatorFunction;
  case DeclKind::PrefixOperator:
    return CodeCompletionDeclKind::PrefixOperatorFunction;
  case DeclKind::PostfixOperator:
    return CodeCompletionDeclKind::PostfixOperatorFunction;
  case DeclKind::PrecedenceGroup:
    return CodeCompletionDeclKind::PrecedenceGroup;
  case DeclKind::EnumElement:
    return CodeCompletionDeclKind::EnumElement;
  case DeclKind::Subscript:
    return CodeCompletionDeclKind::Subscript;
  }
  llvm_unreachable("invalid DeclKind");
}

void CodeCompletionResult::print(raw_ostream &OS) const {
  llvm::SmallString<64> Prefix;
  switch (getKind()) {
  case ResultKind::Declaration:
    Prefix.append("Decl");
    switch (getAssociatedDeclKind()) {
    case CodeCompletionDeclKind::Class:
      Prefix.append("[Class]");
      break;
    case CodeCompletionDeclKind::Struct:
      Prefix.append("[Struct]");
      break;
    case CodeCompletionDeclKind::Enum:
      Prefix.append("[Enum]");
      break;
    case CodeCompletionDeclKind::EnumElement:
      Prefix.append("[EnumElement]");
      break;
    case CodeCompletionDeclKind::Protocol:
      Prefix.append("[Protocol]");
      break;
    case CodeCompletionDeclKind::TypeAlias:
      Prefix.append("[TypeAlias]");
      break;
    case CodeCompletionDeclKind::AssociatedType:
      Prefix.append("[AssociatedType]");
      break;
    case CodeCompletionDeclKind::GenericTypeParam:
      Prefix.append("[GenericTypeParam]");
      break;
    case CodeCompletionDeclKind::Constructor:
      Prefix.append("[Constructor]");
      break;
    case CodeCompletionDeclKind::Destructor:
      Prefix.append("[Destructor]");
      break;
    case CodeCompletionDeclKind::Subscript:
      Prefix.append("[Subscript]");
      break;
    case CodeCompletionDeclKind::StaticMethod:
      Prefix.append("[StaticMethod]");
      break;
    case CodeCompletionDeclKind::InstanceMethod:
      Prefix.append("[InstanceMethod]");
      break;
    case CodeCompletionDeclKind::PrefixOperatorFunction:
      Prefix.append("[PrefixOperatorFunction]");
      break;
    case CodeCompletionDeclKind::PostfixOperatorFunction:
      Prefix.append("[PostfixOperatorFunction]");
      break;
    case CodeCompletionDeclKind::InfixOperatorFunction:
      Prefix.append("[InfixOperatorFunction]");
      break;
    case CodeCompletionDeclKind::FreeFunction:
      Prefix.append("[FreeFunction]");
      break;
    case CodeCompletionDeclKind::StaticVar:
      Prefix.append("[StaticVar]");
      break;
    case CodeCompletionDeclKind::InstanceVar:
      Prefix.append("[InstanceVar]");
      break;
    case CodeCompletionDeclKind::LocalVar:
      Prefix.append("[LocalVar]");
      break;
    case CodeCompletionDeclKind::GlobalVar:
      Prefix.append("[GlobalVar]");
      break;
    case CodeCompletionDeclKind::Module:
      Prefix.append("[Module]");
      break;
    case CodeCompletionDeclKind::PrecedenceGroup:
      Prefix.append("[PrecedenceGroup]");
      break;
    }
    break;
  case ResultKind::Keyword:
    Prefix.append("Keyword");
    switch (getKeywordKind()) {
    case CodeCompletionKeywordKind::None:
      break;
#define KEYWORD(X) case CodeCompletionKeywordKind::kw_##X: \
      Prefix.append("[" #X "]"); \
      break;
#define POUND_KEYWORD(X) case CodeCompletionKeywordKind::pound_##X: \
      Prefix.append("[#" #X "]"); \
      break;
#include "swift/Syntax/TokenKinds.def"
    }
    break;
  case ResultKind::Pattern:
    Prefix.append("Pattern");
    break;
  case ResultKind::Literal:
    Prefix.append("Literal");
    switch (getLiteralKind()) {
    case CodeCompletionLiteralKind::ArrayLiteral:
      Prefix.append("[Array]");
      break;
    case CodeCompletionLiteralKind::BooleanLiteral:
      Prefix.append("[Boolean]");
      break;
    case CodeCompletionLiteralKind::ColorLiteral:
      Prefix.append("[_Color]");
      break;
    case CodeCompletionLiteralKind::ImageLiteral:
      Prefix.append("[_Image]");
      break;
    case CodeCompletionLiteralKind::DictionaryLiteral:
      Prefix.append("[Dictionary]");
      break;
    case CodeCompletionLiteralKind::IntegerLiteral:
      Prefix.append("[Integer]");
      break;
    case CodeCompletionLiteralKind::NilLiteral:
      Prefix.append("[Nil]");
      break;
    case CodeCompletionLiteralKind::StringLiteral:
      Prefix.append("[String]");
      break;
    case CodeCompletionLiteralKind::Tuple:
      Prefix.append("[Tuple]");
      break;
    }
    break;
  case ResultKind::BuiltinOperator:
    Prefix.append("BuiltinOperator");
    break;
  }
  Prefix.append("/");
  switch (getSemanticContext()) {
  case SemanticContextKind::None:
    Prefix.append("None");
    break;
  case SemanticContextKind::ExpressionSpecific:
    Prefix.append("ExprSpecific");
    break;
  case SemanticContextKind::Local:
    Prefix.append("Local");
    break;
  case SemanticContextKind::CurrentNominal:
    Prefix.append("CurrNominal");
    break;
  case SemanticContextKind::Super:
    Prefix.append("Super");
    break;
  case SemanticContextKind::OutsideNominal:
    Prefix.append("OutNominal");
    break;
  case SemanticContextKind::CurrentModule:
    Prefix.append("CurrModule");
    break;
  case SemanticContextKind::OtherModule:
    Prefix.append("OtherModule");
    if (!ModuleName.empty())
      Prefix.append((Twine("[") + ModuleName + "]").str());
    break;
  }
  if (NotRecommended)
    Prefix.append("/NotRecommended");
  if (NumBytesToErase != 0) {
    Prefix.append("/Erase[");
    Prefix.append(Twine(NumBytesToErase).str());
    Prefix.append("]");
  }
  switch (TypeDistance) {
    case ExpectedTypeRelation::Invalid:
      Prefix.append("/TypeRelation[Invalid]");
      break;
    case ExpectedTypeRelation::Identical:
      Prefix.append("/TypeRelation[Identical]");
      break;
    case ExpectedTypeRelation::Convertible:
      Prefix.append("/TypeRelation[Convertible]");
      break;
    case ExpectedTypeRelation::Unrelated:
      break;
  }

  for (clang::comments::WordPairsArrangedViewer Viewer(DocWords);
       Viewer.hasNext();) {
    auto Pair = Viewer.next();
    Prefix.append("/");
    Prefix.append(Pair.first);
    Prefix.append("[");
    StringRef Sep = ", ";
    for (auto KW : Pair.second) {
      Prefix.append(KW);
      Prefix.append(Sep);
    }
    for (unsigned I = 0, N = Sep.size(); I < N; ++I)
      Prefix.pop_back();
    Prefix.append("]");
  }

  Prefix.append(": ");
  while (Prefix.size() < 36) {
    Prefix.append(" ");
  }
  OS << Prefix;
  CompletionString->print(OS);
}

void CodeCompletionResult::dump() const {
  print(llvm::errs());
}

static StringRef copyString(llvm::BumpPtrAllocator &Allocator,
                            StringRef Str) {
  char *Mem = Allocator.Allocate<char>(Str.size());
  std::copy(Str.begin(), Str.end(), Mem);
  return StringRef(Mem, Str.size());
}

static ArrayRef<StringRef> copyStringArray(llvm::BumpPtrAllocator &Allocator,
                                           ArrayRef<StringRef> Arr) {
  StringRef *Buff = Allocator.Allocate<StringRef>(Arr.size());
  std::copy(Arr.begin(), Arr.end(), Buff);
  return llvm::makeArrayRef(Buff, Arr.size());
}

static ArrayRef<std::pair<StringRef, StringRef>> copyStringPairArray(
    llvm::BumpPtrAllocator &Allocator,
    ArrayRef<std::pair<StringRef, StringRef>> Arr) {
  std::pair<StringRef, StringRef> *Buff = Allocator.Allocate<std::pair<StringRef,
    StringRef>>(Arr.size());
  std::copy(Arr.begin(), Arr.end(), Buff);
  return llvm::makeArrayRef(Buff, Arr.size());
}

void CodeCompletionResultBuilder::addChunkWithText(
    CodeCompletionString::Chunk::ChunkKind Kind, StringRef Text) {
  addChunkWithTextNoCopy(Kind, copyString(*Sink.Allocator, Text));
}

void CodeCompletionResultBuilder::setAssociatedDecl(const Decl *D) {
  assert(Kind == CodeCompletionResult::ResultKind::Declaration);
  AssociatedDecl = D;

  if (auto *ClangD = D->getClangDecl())
    CurrentModule = ClangD->getImportedOwningModule();
  // FIXME: macros
  // FIXME: imported header module

  if (!CurrentModule)
    CurrentModule = D->getModuleContext();

  if (D->getAttrs().getDeprecated(D->getASTContext()))
    setNotRecommended(CodeCompletionResult::Deprecated);
}

StringRef CodeCompletionContext::copyString(StringRef Str) {
  return ::copyString(*CurrentResults.Allocator, Str);
}

bool shouldCopyAssociatedUSRForDecl(const ValueDecl *VD) {
  // Avoid trying to generate a USR for some declaration types.
  if (isa<AbstractTypeParamDecl>(VD) && !isa<AssociatedTypeDecl>(VD))
    return false;
  if (isa<ParamDecl>(VD))
    return false;
  if (isa<ModuleDecl>(VD))
    return false;
  if (VD->hasClangNode() && !VD->getClangDecl())
    return false;

  return true;
}

template <typename FnTy>
static void walkValueDeclAndOverriddenDecls(const Decl *D, const FnTy &Fn) {
  if (auto *VD = dyn_cast<ValueDecl>(D)) {
    Fn(VD);
    walkOverriddenDecls(VD, Fn);
  }
}

ArrayRef<StringRef> copyAssociatedUSRs(llvm::BumpPtrAllocator &Allocator,
                                       const Decl *D) {
  llvm::SmallVector<StringRef, 4> USRs;
  walkValueDeclAndOverriddenDecls(D, [&](llvm::PointerUnion<const ValueDecl*,
                                                  const clang::NamedDecl*> OD) {
    llvm::SmallString<128> SS;
    bool Ignored = true;
    if (auto *OVD = OD.dyn_cast<const ValueDecl*>()) {
      if (shouldCopyAssociatedUSRForDecl(OVD)) {
        llvm::raw_svector_ostream OS(SS);
        Ignored = printDeclUSR(OVD, OS);
      }
    } else if (auto *OND = OD.dyn_cast<const clang::NamedDecl*>()) {
      Ignored = clang::index::generateUSRForDecl(OND, SS);
    }

    if (!Ignored)
      USRs.push_back(copyString(Allocator, SS));
  });

  if (!USRs.empty())
    return copyStringArray(Allocator, USRs);

  return ArrayRef<StringRef>();
}

static CodeCompletionResult::ExpectedTypeRelation calculateTypeRelation(
                                                                Type Ty,
                                                                Type ExpectedTy,
                                                                DeclContext *DC) {
  if (Ty.isNull() || ExpectedTy.isNull() ||
      Ty->is<ErrorType>() ||
      ExpectedTy->is<ErrorType>())
    return CodeCompletionResult::ExpectedTypeRelation::Unrelated;

  // Equality/Conversion of GenericTypeParameterType won't account for
  // requirements – ignore them
  if (!Ty->hasTypeParameter() && !ExpectedTy->hasTypeParameter()) {
    if (Ty->isEqual(ExpectedTy))
      return CodeCompletionResult::ExpectedTypeRelation::Identical;
    if (!ExpectedTy->isAny() && isConvertibleTo(Ty, ExpectedTy, *DC))
      return CodeCompletionResult::ExpectedTypeRelation::Convertible;
  }
  if (auto FT = Ty->getAs<AnyFunctionType>()) {
    if (FT->getResult()->isVoid())
      return CodeCompletionResult::ExpectedTypeRelation::Invalid;
  }
  return CodeCompletionResult::ExpectedTypeRelation::Unrelated;
}

static CodeCompletionResult::ExpectedTypeRelation
calculateTypeRelationForDecl(const Decl *D, Type ExpectedType,
                             bool IsImplicitlyCurriedInstanceMethod,
                             bool UseFuncResultType = true) {
  auto VD = dyn_cast<ValueDecl>(D);
  auto DC = D->getDeclContext();
  if (!VD || !VD->hasInterfaceType())
    return CodeCompletionResult::ExpectedTypeRelation::Unrelated;

  if (auto FD = dyn_cast<AbstractFunctionDecl>(VD)) {
    auto funcType = FD->getInterfaceType()->getAs<AnyFunctionType>();
    if (DC->isTypeContext() && funcType && funcType->is<AnyFunctionType>() &&
        !IsImplicitlyCurriedInstanceMethod)
      funcType = funcType->getResult()->getAs<AnyFunctionType>();
    if (funcType) {
      funcType = funcType->removeArgumentLabels(1)->castTo<AnyFunctionType>();
      auto relation = calculateTypeRelation(funcType, ExpectedType, DC);
      if (UseFuncResultType)
        relation =
            std::max(relation, calculateTypeRelation(funcType->getResult(),
                                                     ExpectedType, DC));
      return relation;
    }
  }
  if (auto NTD = dyn_cast<NominalTypeDecl>(VD)) {
    return std::max(
        calculateTypeRelation(NTD->getInterfaceType(), ExpectedType, DC),
        calculateTypeRelation(NTD->getDeclaredInterfaceType(), ExpectedType, DC));
  }
  return calculateTypeRelation(VD->getInterfaceType(), ExpectedType, DC);
}

static CodeCompletionResult::ExpectedTypeRelation
calculateMaxTypeRelationForDecl(
    const Decl *D,
    ArrayRef<Type> ExpectedTypes,
    bool IsImplicitlyCurriedInstanceMethod = false) {
  auto Result = CodeCompletionResult::ExpectedTypeRelation::Unrelated;
  for (auto Type : ExpectedTypes) {
    Result = std::max(Result, calculateTypeRelationForDecl(
                                  D, Type, IsImplicitlyCurriedInstanceMethod));
  }
  return Result;
}

CodeCompletionOperatorKind
CodeCompletionResult::getCodeCompletionOperatorKind(StringRef name) {
  using CCOK = CodeCompletionOperatorKind;
  using OpPair = std::pair<StringRef, CCOK>;

  // This list must be kept in alphabetical order.
  static OpPair ops[] = {
      std::make_pair("!", CCOK::Bang),
      std::make_pair("!=", CCOK::NotEq),
      std::make_pair("!==", CCOK::NotEqEq),
      std::make_pair("%", CCOK::Modulo),
      std::make_pair("%=", CCOK::ModuloEq),
      std::make_pair("&", CCOK::Amp),
      std::make_pair("&&", CCOK::AmpAmp),
      std::make_pair("&*", CCOK::AmpStar),
      std::make_pair("&+", CCOK::AmpPlus),
      std::make_pair("&-", CCOK::AmpMinus),
      std::make_pair("&=", CCOK::AmpEq),
      std::make_pair("(", CCOK::LParen),
      std::make_pair("*", CCOK::Star),
      std::make_pair("*=", CCOK::StarEq),
      std::make_pair("+", CCOK::Plus),
      std::make_pair("+=", CCOK::PlusEq),
      std::make_pair("-", CCOK::Minus),
      std::make_pair("-=", CCOK::MinusEq),
      std::make_pair(".", CCOK::Dot),
      std::make_pair("...", CCOK::DotDotDot),
      std::make_pair("..<", CCOK::DotDotLess),
      std::make_pair("/", CCOK::Slash),
      std::make_pair("/=", CCOK::SlashEq),
      std::make_pair("<", CCOK::Less),
      std::make_pair("<<", CCOK::LessLess),
      std::make_pair("<<=", CCOK::LessLessEq),
      std::make_pair("<=", CCOK::LessEq),
      std::make_pair("=", CCOK::Eq),
      std::make_pair("==", CCOK::EqEq),
      std::make_pair("===", CCOK::EqEqEq),
      std::make_pair(">", CCOK::Greater),
      std::make_pair(">=", CCOK::GreaterEq),
      std::make_pair(">>", CCOK::GreaterGreater),
      std::make_pair(">>=", CCOK::GreaterGreaterEq),
      std::make_pair("?.", CCOK::QuestionDot),
      std::make_pair("^", CCOK::Caret),
      std::make_pair("^=", CCOK::CaretEq),
      std::make_pair("|", CCOK::Pipe),
      std::make_pair("|=", CCOK::PipeEq),
      std::make_pair("||", CCOK::PipePipe),
      std::make_pair("~=", CCOK::TildeEq),
  };
  static auto opsSize = sizeof(ops) / sizeof(ops[0]);

  auto I = std::lower_bound(
      ops, &ops[opsSize], std::make_pair(name, CCOK::None),
      [](const OpPair &a, const OpPair &b) { return a.first < b.first; });

  if (I == &ops[opsSize] || I->first != name)
    return CCOK::Unknown;
  return I->second;
}

static StringRef getOperatorName(CodeCompletionString *str) {
  return str->getFirstTextChunk(/*includeLeadingPunctuation=*/true);
}

CodeCompletionOperatorKind
CodeCompletionResult::getCodeCompletionOperatorKind(CodeCompletionString *str) {
  return getCodeCompletionOperatorKind(getOperatorName(str));
}

CodeCompletionResult *CodeCompletionResultBuilder::takeResult() {
  auto *CCS = CodeCompletionString::create(*Sink.Allocator, Chunks);

  switch (Kind) {
  case CodeCompletionResult::ResultKind::Declaration: {
    StringRef BriefComment;
    auto MaybeClangNode = AssociatedDecl->getClangNode();
    if (MaybeClangNode) {
      if (auto *D = MaybeClangNode.getAsDecl()) {
        const auto &ClangContext = D->getASTContext();
        if (const clang::RawComment *RC =
                ClangContext.getRawCommentForAnyRedecl(D))
          BriefComment = RC->getBriefText(ClangContext);
      }
    } else {
      BriefComment = AssociatedDecl->getBriefComment();
    }

    StringRef ModuleName;
    if (CurrentModule) {
      if (Sink.LastModule.first == CurrentModule.getOpaqueValue()) {
        ModuleName = Sink.LastModule.second;
      } else {
        if (auto *C = CurrentModule.dyn_cast<const clang::Module *>()) {
          ModuleName = copyString(*Sink.Allocator, C->getFullModuleName());
        } else {
          ModuleName = copyString(
              *Sink.Allocator,
              CurrentModule.get<const swift::ModuleDecl *>()->getName().str());
        }
        Sink.LastModule.first = CurrentModule.getOpaqueValue();
        Sink.LastModule.second = ModuleName;
      }
    }

    auto typeRelation = ExpectedTypeRelation;
    if (typeRelation == CodeCompletionResult::Unrelated)
      typeRelation =
          calculateMaxTypeRelationForDecl(AssociatedDecl, ExpectedDeclTypes);

    if (typeRelation == CodeCompletionResult::Invalid) {
      IsNotRecommended = true;
      NotRecReason = CodeCompletionResult::NotRecommendedReason::TypeMismatch;
    }

    return new (*Sink.Allocator) CodeCompletionResult(
        SemanticContext, NumBytesToErase, CCS, AssociatedDecl, ModuleName,
        /*NotRecommended=*/IsNotRecommended, NotRecReason,
        copyString(*Sink.Allocator, BriefComment),
        copyAssociatedUSRs(*Sink.Allocator, AssociatedDecl),
        copyStringPairArray(*Sink.Allocator, CommentWords), typeRelation);
  }

  case CodeCompletionResult::ResultKind::Keyword:
    return new (*Sink.Allocator)
        CodeCompletionResult(KeywordKind, SemanticContext, NumBytesToErase,
                             CCS, ExpectedTypeRelation);

  case CodeCompletionResult::ResultKind::BuiltinOperator:
  case CodeCompletionResult::ResultKind::Pattern:
    return new (*Sink.Allocator) CodeCompletionResult(
        Kind, SemanticContext, NumBytesToErase, CCS, ExpectedTypeRelation);

  case CodeCompletionResult::ResultKind::Literal:
    assert(LiteralKind.hasValue());
    return new (*Sink.Allocator)
        CodeCompletionResult(*LiteralKind, SemanticContext, NumBytesToErase,
                             CCS, ExpectedTypeRelation);
  }

  llvm_unreachable("Unhandled CodeCompletionResult in switch.");
}

void CodeCompletionResultBuilder::finishResult() {
  if (!Cancelled)
    Sink.Results.push_back(takeResult());
}


MutableArrayRef<CodeCompletionResult *> CodeCompletionContext::takeResults() {
  // Copy pointers to the results.
  const size_t Count = CurrentResults.Results.size();
  CodeCompletionResult **Results =
      CurrentResults.Allocator->Allocate<CodeCompletionResult *>(Count);
  std::copy(CurrentResults.Results.begin(), CurrentResults.Results.end(),
            Results);
  CurrentResults.Results.clear();
  return MutableArrayRef<CodeCompletionResult *>(Results, Count);
}

Optional<unsigned> CodeCompletionString::getFirstTextChunkIndex(
    bool includeLeadingPunctuation) const {
  for (auto i : indices(getChunks())) {
    auto &C = getChunks()[i];
    switch (C.getKind()) {
    using ChunkKind = Chunk::ChunkKind;
    case ChunkKind::Text:
    case ChunkKind::CallParameterName:
    case ChunkKind::CallParameterInternalName:
    case ChunkKind::GenericParameterName:
    case ChunkKind::LeftParen:
    case ChunkKind::LeftBracket:
    case ChunkKind::Equal:
    case ChunkKind::DeclAttrParamKeyword:
    case ChunkKind::DeclAttrKeyword:
      return i;
    case ChunkKind::Dot:
    case ChunkKind::ExclamationMark:
    case ChunkKind::QuestionMark:
      if (includeLeadingPunctuation)
        return i;
      continue;
    case ChunkKind::RightParen:
    case ChunkKind::RightBracket:
    case ChunkKind::LeftAngle:
    case ChunkKind::RightAngle:
    case ChunkKind::Ellipsis:
    case ChunkKind::Comma:
    case ChunkKind::Ampersand:
    case ChunkKind::Whitespace:
    case ChunkKind::AccessControlKeyword:
    case ChunkKind::OverrideKeyword:
    case ChunkKind::ThrowsKeyword:
    case ChunkKind::RethrowsKeyword:
    case ChunkKind::DeclIntroducer:
    case ChunkKind::CallParameterColon:
    case ChunkKind::DeclAttrParamColon:
    case ChunkKind::CallParameterType:
    case ChunkKind::CallParameterClosureType:
    case ChunkKind::OptionalBegin:
    case ChunkKind::CallParameterBegin:
    case ChunkKind::GenericParameterBegin:
    case ChunkKind::DynamicLookupMethodCallTail:
    case ChunkKind::OptionalMethodCallTail:
    case ChunkKind::TypeAnnotation:
      continue;

    case ChunkKind::BraceStmtWithCursor:
      llvm_unreachable("should have already extracted the text");
    }
  }
  return None;
}

StringRef
CodeCompletionString::getFirstTextChunk(bool includeLeadingPunctuation) const {
  Optional<unsigned> Idx = getFirstTextChunkIndex(includeLeadingPunctuation);
  if (Idx.hasValue())
    return getChunks()[*Idx].getText();
  return StringRef();
}

void CodeCompletionString::getName(raw_ostream &OS) const {
  auto FirstTextChunk = getFirstTextChunkIndex();
  int TextSize = 0;
  if (FirstTextChunk.hasValue()) {
    for (auto C : getChunks().slice(*FirstTextChunk)) {
      using ChunkKind = Chunk::ChunkKind;

      bool shouldPrint = !C.isAnnotation();
      switch (C.getKind()) {
      case ChunkKind::TypeAnnotation:
      case ChunkKind::CallParameterClosureType:
      case ChunkKind::DeclAttrParamColon:
      case ChunkKind::OptionalMethodCallTail:
        continue;
      case ChunkKind::ThrowsKeyword:
      case ChunkKind::RethrowsKeyword:
        shouldPrint = true; // Even when they're annotations.
        break;
      default:
        break;
      }

      if (C.hasText() && shouldPrint) {
        TextSize += C.getText().size();
        OS << C.getText();
      }
    }
  }
  assert((TextSize > 0) &&
         "code completion string should have non-empty name!");
}

void CodeCompletionContext::sortCompletionResults(
    MutableArrayRef<CodeCompletionResult *> Results) {
  struct ResultAndName {
    CodeCompletionResult *result;
    std::string name;
  };

  // Caching the name of each field is important to avoid unnecessary calls to
  // CodeCompletionString::getName().
  std::vector<ResultAndName> nameCache(Results.size());
  for (unsigned i = 0, n = Results.size(); i < n; ++i) {
    auto *result = Results[i];
    nameCache[i].result = result;
    llvm::raw_string_ostream OS(nameCache[i].name);
    result->getCompletionString()->getName(OS);
    OS.flush();
  }

  // Sort nameCache, and then transform Results to return the pointers in order.
  std::sort(nameCache.begin(), nameCache.end(),
            [](const ResultAndName &LHS, const ResultAndName &RHS) {
    int Result = StringRef(LHS.name).compare_lower(RHS.name);
    // If the case insensitive comparison is equal, then secondary sort order
    // should be case sensitive.
    if (Result == 0)
      Result = LHS.name.compare(RHS.name);
    return Result < 0;
  });

  std::transform(nameCache.begin(), nameCache.end(), Results.begin(),
                 [](const ResultAndName &entry) { return entry.result; });
}

namespace {
class CodeCompletionCallbacksImpl : public CodeCompletionCallbacks {
  CodeCompletionContext &CompletionContext;
  std::vector<RequestedCachedModule> RequestedModules;
  CodeCompletionConsumer &Consumer;
  CodeCompletionExpr *CodeCompleteTokenExpr = nullptr;
  AssignExpr *AssignmentExpr;
  CompletionKind Kind = CompletionKind::None;
  Expr *ParsedExpr = nullptr;
  SourceLoc DotLoc;
  TypeLoc ParsedTypeLoc;
  DeclContext *CurDeclContext = nullptr;
  DeclAttrKind AttrKind;

  /// In situations when \c SyntaxKind hints or determines
  /// completions, i.e. a precedence group attribute, this
  /// can be set and used to control the code completion scenario.
  SyntaxKind SyntxKind;

  int AttrParamIndex;
  bool IsInSil = false;
  bool HasSpace = false;
  bool ShouldCompleteCallPatternAfterParen = true;
  bool PreferFunctionReferencesToCalls = false;
  Optional<DeclKind> AttTargetDK;
  Optional<StmtKind> ParentStmtKind;

  SmallVector<StringRef, 3> ParsedKeywords;

  std::vector<std::pair<std::string, bool>> SubModuleNameVisibilityPairs;

  void addSuperKeyword(CodeCompletionResultSink &Sink) {
    auto *DC = CurDeclContext->getInnermostTypeContext();
    if (!DC)
      return;
    auto *CD = DC->getSelfClassDecl();
    if (!CD)
      return;
    Type ST = CD->getSuperclass();
    if (ST.isNull() || ST->is<ErrorType>())
      return;

    CodeCompletionResultBuilder Builder(Sink,
                                        CodeCompletionResult::ResultKind::Keyword,
                                        SemanticContextKind::CurrentNominal,
                                        {});
    Builder.setKeywordKind(CodeCompletionKeywordKind::kw_super);
    Builder.addTextChunk("super");
    Builder.addTypeAnnotation(ST.getString());
  }

  /// Set to true when we have delivered code completion results
  /// to the \c Consumer.
  bool DeliveredResults = false;


  Optional<std::pair<Type, ConcreteDeclRef>> typeCheckParsedExpr() {
    assert(ParsedExpr && "should have an expression");

    // Figure out the kind of type-check we'll be performing.
    auto CheckKind = CompletionTypeCheckKind::Normal;
    if (Kind == CompletionKind::KeyPathExprObjC)
      CheckKind = CompletionTypeCheckKind::KeyPath;

    // If we've already successfully type-checked the expression for some
    // reason, just return the type.
    // FIXME: if it's ErrorType but we've already typechecked we shouldn't
    // typecheck again. rdar://21466394
    if (CheckKind == CompletionTypeCheckKind::Normal &&
        ParsedExpr->getType() && !ParsedExpr->getType()->is<ErrorType>())
      return std::make_pair(ParsedExpr->getType(),
                            ParsedExpr->getReferencedDecl());

    prepareForRetypechecking(ParsedExpr);

    ConcreteDeclRef ReferencedDecl = nullptr;
    Expr *ModifiedExpr = ParsedExpr;
    if (auto T = getTypeOfCompletionContextExpr(P.Context, CurDeclContext,
                                                CheckKind, ModifiedExpr,
                                                ReferencedDecl)) {
      // FIXME: even though we don't apply the solution, the type checker may
      // modify the original expression. We should understand what effect that
      // may have on code completion.
      ParsedExpr = ModifiedExpr;

      return std::make_pair(*T, ReferencedDecl);
    }
    return None;
  }

  /// \returns true on success, false on failure.
  bool typecheckParsedType() {
    assert(ParsedTypeLoc.getTypeRepr() && "should have a TypeRepr");
    return !performTypeLocChecking(P.Context, ParsedTypeLoc,
                                   CurDeclContext, false);
  }

public:
  CodeCompletionCallbacksImpl(Parser &P,
                              CodeCompletionContext &CompletionContext,
                              CodeCompletionConsumer &Consumer)
      : CodeCompletionCallbacks(P), CompletionContext(CompletionContext),
        Consumer(Consumer) {
  }

  void completeExpr() override;
  void completeDotExpr(Expr *E, SourceLoc DotLoc) override;
  void completeStmtOrExpr() override;
  void completePostfixExprBeginning(CodeCompletionExpr *E) override;
  void completeForEachSequenceBeginning(CodeCompletionExpr *E) override;
  void completePostfixExpr(Expr *E, bool hasSpace) override;
  void completePostfixExprParen(Expr *E, Expr *CodeCompletionE) override;
  void completeExprSuper(SuperRefExpr *SRE) override;
  void completeExprSuperDot(SuperRefExpr *SRE) override;
  void completeExprKeyPath(KeyPathExpr *KPE, SourceLoc DotLoc) override;

  void completeTypeSimpleBeginning() override;
  void completeTypeIdentifierWithDot(IdentTypeRepr *ITR) override;
  void completeTypeIdentifierWithoutDot(IdentTypeRepr *ITR) override;

  void completeCaseStmtBeginning() override;
  void completeCaseStmtDotPrefix() override;
  void completeDeclAttrKeyword(Decl *D, bool Sil, bool Param) override;
  void completeDeclAttrParam(DeclAttrKind DK, int Index) override;
  void completeInPrecedenceGroup(SyntaxKind SK) override;
  void completeNominalMemberBeginning(
      SmallVectorImpl<StringRef> &Keywords) override;
  void completeAccessorBeginning() override;

  void completePoundAvailablePlatform() override;
  void completeImportDecl(std::vector<std::pair<Identifier, SourceLoc>> &Path) override;
  void completeUnresolvedMember(CodeCompletionExpr *E,
                                SourceLoc DotLoc) override;
  void completeAssignmentRHS(AssignExpr *E) override;
  void completeCallArg(CodeCompletionExpr *E) override;
  void completeReturnStmt(CodeCompletionExpr *E) override;
  void completeYieldStmt(CodeCompletionExpr *E,
                         Optional<unsigned> yieldIndex) override;
  void completeAfterPoundExpr(CodeCompletionExpr *E,
                              Optional<StmtKind> ParentKind) override;
  void completeAfterPoundDirective() override;
  void completePlatformCondition() override;
  void completeGenericParams(TypeLoc TL) override;
  void completeAfterIfStmt(bool hasElse) override;

  void doneParsing() override;

private:
  void addKeywords(CodeCompletionResultSink &Sink, bool MaybeFuncBody);
  void deliverCompletionResults();
};
} // end anonymous namespace

void CodeCompletionCallbacksImpl::completeExpr() {
  if (DeliveredResults)
    return;

  Parser::ParserPositionRAII RestorePosition(P);
  P.restoreParserPosition(ExprBeginPosition);

  // FIXME: implement fallback code completion.

  deliverCompletionResults();
}

namespace {
static bool isTopLevelContext(const DeclContext *DC) {
  for (; DC && DC->isLocalContext(); DC = DC->getParent()) {
    switch (DC->getContextKind()) {
    case DeclContextKind::TopLevelCodeDecl:
      return true;
    case DeclContextKind::AbstractFunctionDecl:
    case DeclContextKind::SubscriptDecl:
      return false;
    default:
      continue;
    }
  }
  return false;
}

static KnownProtocolKind
protocolForLiteralKind(CodeCompletionLiteralKind kind) {
  switch (kind) {
  case CodeCompletionLiteralKind::ArrayLiteral:
    return KnownProtocolKind::ExpressibleByArrayLiteral;
  case CodeCompletionLiteralKind::BooleanLiteral:
    return KnownProtocolKind::ExpressibleByBooleanLiteral;
  case CodeCompletionLiteralKind::ColorLiteral:
    return KnownProtocolKind::ExpressibleByColorLiteral;
  case CodeCompletionLiteralKind::ImageLiteral:
    return KnownProtocolKind::ExpressibleByImageLiteral;
  case CodeCompletionLiteralKind::DictionaryLiteral:
    return KnownProtocolKind::ExpressibleByDictionaryLiteral;
  case CodeCompletionLiteralKind::IntegerLiteral:
    return KnownProtocolKind::ExpressibleByIntegerLiteral;
  case CodeCompletionLiteralKind::NilLiteral:
    return KnownProtocolKind::ExpressibleByNilLiteral;
  case CodeCompletionLiteralKind::StringLiteral:
    return KnownProtocolKind::ExpressibleByUnicodeScalarLiteral;
  case CodeCompletionLiteralKind::Tuple:
    llvm_unreachable("no such protocol kind");
  }

  llvm_unreachable("Unhandled CodeCompletionLiteralKind in switch.");
}

/// Whether funcType has a single argument (not including defaulted arguments)
/// that is of type () -> ().
static bool hasTrivialTrailingClosure(const FuncDecl *FD,
                                      AnyFunctionType *funcType) {
  SmallBitVector defaultMap =
    computeDefaultMap(funcType->getParams(), FD,
                      /*level*/ FD->isInstanceMember() ? 1 : 0);
  
  if (defaultMap.size() - defaultMap.count() == 1) {
    auto param = funcType->getParams().back();
    if (!param.isAutoClosure()) {
      if (auto Fn = param.getOldType()->getAs<AnyFunctionType>()) {
        return Fn->getParams().empty() && Fn->getResult()->isVoid();
      }
    }
  }

  return false;
}

/// Build completions by doing visible decl lookup from a context.
class CompletionLookup final : public swift::VisibleDeclConsumer {
  CodeCompletionResultSink &Sink;
  ASTContext &Ctx;
  LazyResolver *TypeResolver = nullptr;
  const DeclContext *CurrDeclContext;
  ClangImporter *Importer;
  CodeCompletionContext *CompletionContext;

  enum class LookupKind {
    ValueExpr,
    ValueInDeclContext,
    EnumElement,
    Type,
    TypeInDeclContext,
    ImportFromModule
  };

  LookupKind Kind;

  /// Type of the user-provided expression for LookupKind::ValueExpr
  /// completions.
  Type ExprType;

  /// Whether the expr is of statically inferred metatype.
  bool IsStaticMetatype = false;

  /// User-provided base type for LookupKind::Type completions.
  Type BaseType;

  /// Expected types of the code completion expression.
  std::vector<Type> ExpectedTypes;

  bool HaveDot = false;
  bool IsUnwrappedOptional = false;
  SourceLoc DotLoc;
  bool NeedLeadingDot = false;

  bool NeedOptionalUnwrap = false;
  unsigned NumBytesToEraseForOptionalUnwrap = 0;

  bool HaveLParen = false;
  bool IsSuperRefExpr = false;
  bool IsSelfRefExpr = false;
  bool IsKeyPathExpr = false;
  bool IsSwiftKeyPathExpr = false;
  bool IsAfterSwiftKeyPathRoot = false;
  bool IsDynamicLookup = false;
  bool PreferFunctionReferencesToCalls = false;
  bool HaveLeadingSpace = false;

  bool IncludeInstanceMembers = false;

  /// True if we are code completing inside a static method.
  bool InsideStaticMethod = false;

  /// Innermost method that the code completion point is in.
  const AbstractFunctionDecl *CurrentMethod = nullptr;

  Optional<SemanticContextKind> ForcedSemanticContext = None;
  bool IsUnresolvedMember = false;

public:
  bool FoundFunctionCalls = false;
  bool FoundFunctionsWithoutFirstKeyword = false;

private:
  void foundFunction(const AbstractFunctionDecl *AFD) {
    FoundFunctionCalls = true;
    DeclName Name = AFD->getFullName();
    auto ArgNames = Name.getArgumentNames();
    if (ArgNames.empty())
      return;
    if (ArgNames[0].empty())
      FoundFunctionsWithoutFirstKeyword = true;
  }

  void foundFunction(const AnyFunctionType *AFT) {
    FoundFunctionCalls = true;
    auto Params = AFT->getParams();
    if (Params.empty())
      return;
    if (Params.size() == 1 && !Params[0].hasLabel()) {
      FoundFunctionsWithoutFirstKeyword = true;
      return;
    }
    if (!Params[0].hasLabel())
      FoundFunctionsWithoutFirstKeyword = true;
  }

  void setClangDeclKeywords(const ValueDecl *VD, CommandWordsPairs &Pairs,
                            CodeCompletionResultBuilder &Builder) {
    if (auto *CD = VD->getClangDecl()) {
      clang::comments::getClangDocKeyword(*Importer, CD, Pairs);
    } else {
      swift::markup::getSwiftDocKeyword(VD, Pairs);
    }
    Builder.addDeclDocCommentWords(llvm::makeArrayRef(Pairs));
  }

  bool shouldUseFunctionReference(AbstractFunctionDecl *D) {
    if (PreferFunctionReferencesToCalls)
      return true;
    bool isImplicitlyCurriedIM = isImplicitlyCurriedInstanceMethod(D);
    for (auto expectedType : ExpectedTypes) {
      if (expectedType &&
          expectedType->lookThroughAllOptionalTypes()
              ->is<AnyFunctionType>() &&
          calculateTypeRelationForDecl(D, expectedType, isImplicitlyCurriedIM,
                                       /*UseFuncResultType=*/false) >=
              CodeCompletionResult::ExpectedTypeRelation::Convertible) {
        return true;
      }
    }
    return false;
  }

public:
  struct RequestedResultsTy {
    const ModuleDecl *TheModule;
    bool OnlyTypes;
    bool OnlyPrecedenceGroups;
    bool NeedLeadingDot;

    static RequestedResultsTy fromModule(const ModuleDecl *TheModule) {
      return { TheModule, false, false, false };
    }

    RequestedResultsTy onlyTypes() const {
      return { TheModule, true, false, NeedLeadingDot };
    }

    RequestedResultsTy onlyPrecedenceGroups() const {
      assert(!OnlyTypes && "onlyTypes() already includes precedence groups");
      return { TheModule, false, true, false };
    }

    RequestedResultsTy needLeadingDot(bool NeedDot) const {
      return { TheModule, OnlyTypes, OnlyPrecedenceGroups, NeedDot };
    }

    static RequestedResultsTy toplevelResults() {
      return { nullptr, false, false, false };
    }
  };

  std::vector<RequestedResultsTy> RequestedCachedResults;

public:
  CompletionLookup(CodeCompletionResultSink &Sink,
                   ASTContext &Ctx,
                   const DeclContext *CurrDeclContext,
                   CodeCompletionContext *CompletionContext = nullptr)
      : Sink(Sink), Ctx(Ctx), CurrDeclContext(CurrDeclContext),
        Importer(static_cast<ClangImporter *>(CurrDeclContext->getASTContext().
          getClangModuleLoader())),
        CompletionContext(CompletionContext) {
    (void)createTypeChecker(Ctx);
    TypeResolver = Ctx.getLazyResolver();

    // Determine if we are doing code completion inside a static method.
    if (CurrDeclContext) {
      CurrentMethod = CurrDeclContext->getInnermostMethodContext();
      if (auto *FD = dyn_cast_or_null<FuncDecl>(CurrentMethod))
        InsideStaticMethod = FD->isStatic();
    }
  }

  void setHaveDot(SourceLoc DotLoc) {
    HaveDot = true;
    this->DotLoc = DotLoc;
  }

  void setIsUnwrappedOptional(bool value) {
    IsUnwrappedOptional = value;
  }

  void setIsStaticMetatype(bool value) {
    IsStaticMetatype = value;
  }

  void setExpectedTypes(ArrayRef<Type> Types) {
    ExpectedTypes.reserve(Types.size());
    for (auto T : Types)
      if (T)
        ExpectedTypes.push_back(T);
  }

  bool hasExpectedTypes() const { return !ExpectedTypes.empty(); }

  bool needDot() const {
    return NeedLeadingDot;
  }

  void setHaveLParen(bool Value) {
    HaveLParen = Value;
  }

  void setIsSuperRefExpr() {
    IsSuperRefExpr = true;
  }

  void setIsSelfRefExpr(bool value) { IsSelfRefExpr = value; }

  void setIsKeyPathExpr() {
    IsKeyPathExpr = true;
  }

  void setIsSwiftKeyPathExpr(bool onRoot) {
    IsSwiftKeyPathExpr = true;
    IsAfterSwiftKeyPathRoot = onRoot;
  }

  void setIsDynamicLookup() {
    IsDynamicLookup = true;
  }

  void setPreferFunctionReferencesToCalls() {
    PreferFunctionReferencesToCalls = true;
  }

  void setHaveLeadingSpace(bool value) { HaveLeadingSpace = value; }

  void includeInstanceMembers() {
    IncludeInstanceMembers = true;
  }

  void addSubModuleNames(std::vector<std::pair<std::string, bool>>
      &SubModuleNameVisibilityPairs) {
    for (auto &Pair : SubModuleNameVisibilityPairs) {
      CodeCompletionResultBuilder Builder(Sink,
                                          CodeCompletionResult::ResultKind::
                                          Declaration,
                                          SemanticContextKind::OtherModule,
                                          ExpectedTypes);
      auto MD = ModuleDecl::create(Ctx.getIdentifier(Pair.first), Ctx);
      Builder.setAssociatedDecl(MD);
      Builder.addTextChunk(MD->getNameStr());
      Builder.addTypeAnnotation("Module");
      if (Pair.second)
        Builder.setNotRecommended(CodeCompletionResult::NotRecommendedReason::
                                    Redundant);
    }
  }

  void collectImportedModules(llvm::StringSet<> &ImportedModules) {
    SmallVector<ModuleDecl::ImportedModule, 16> Imported;
    SmallVector<ModuleDecl::ImportedModule, 16> FurtherImported;
    CurrDeclContext->getParentSourceFile()->getImportedModules(Imported,
      ModuleDecl::ImportFilter::All);
    while (!Imported.empty()) {
      ModuleDecl *MD = Imported.back().second;
      Imported.pop_back();
      if (!ImportedModules.insert(MD->getNameStr()).second)
        continue;
      FurtherImported.clear();
      MD->getImportedModules(FurtherImported, ModuleDecl::ImportFilter::Public);
      Imported.append(FurtherImported.begin(), FurtherImported.end());
      for (auto SubMod : FurtherImported) {
        Imported.push_back(SubMod);
      }
    }
  }

  void addImportModuleNames() {
    // FIXME: Add user-defined swift modules
    SmallVector<StringRef, 20> ModuleNames;

    // Collect clang module names.
    {
      SmallVector<clang::Module*, 20> ClangModules;
      Ctx.getVisibleTopLevelClangModules(ClangModules);
      for (auto *M : ClangModules) {
        if (!M->isAvailable())
          continue;
        if (M->getTopLevelModuleName().startswith("_"))
          continue;
        if (M->getTopLevelModuleName() == Ctx.SwiftShimsModuleName.str())
          continue;

        ModuleNames.push_back(M->getTopLevelModuleName());
      }
    }

    std::sort(ModuleNames.begin(), ModuleNames.end(),
              [](StringRef LHS, StringRef RHS) {
                return LHS.compare_lower(RHS) < 0;
              });

    llvm::StringSet<> ImportedModules;
    collectImportedModules(ImportedModules);

    for (auto ModuleName : ModuleNames) {
      auto MD = ModuleDecl::create(Ctx.getIdentifier(ModuleName), Ctx);
      CodeCompletionResultBuilder Builder(
          Sink,
          CodeCompletionResult::ResultKind::Declaration,
          SemanticContextKind::OtherModule,
          ExpectedTypes);
      Builder.setAssociatedDecl(MD);
      Builder.addTextChunk(MD->getNameStr());
      Builder.addTypeAnnotation("Module");

      // Imported modules are not recommended.
      if (ImportedModules.count(MD->getNameStr()) != 0)
        Builder.setNotRecommended(
            CodeCompletionResult::NotRecommendedReason::Redundant);
    }
  }

  SemanticContextKind getSemanticContext(const Decl *D,
                                         DeclVisibilityKind Reason) {
    if (ForcedSemanticContext)
      return *ForcedSemanticContext;

    if (IsUnresolvedMember) {
      if (isa<EnumElementDecl>(D)) {
        return SemanticContextKind::ExpressionSpecific;
      }
    }

    switch (Reason) {
    case DeclVisibilityKind::LocalVariable:
    case DeclVisibilityKind::FunctionParameter:
    case DeclVisibilityKind::GenericParameter:
      return SemanticContextKind::Local;

    case DeclVisibilityKind::MemberOfCurrentNominal:
      if (IsSuperRefExpr &&
          CurrentMethod && CurrentMethod->getOverriddenDecl() == D)
        return SemanticContextKind::ExpressionSpecific;
      return SemanticContextKind::CurrentNominal;

    case DeclVisibilityKind::MemberOfProtocolImplementedByCurrentNominal:
    case DeclVisibilityKind::MemberOfSuper:
      return SemanticContextKind::Super;

    case DeclVisibilityKind::MemberOfOutsideNominal:
      return SemanticContextKind::OutsideNominal;

    case DeclVisibilityKind::VisibleAtTopLevel:
      if (CurrDeclContext &&
          D->getModuleContext() == CurrDeclContext->getParentModule()) {
        // Treat global variables from the same source file as local when
        // completing at top-level.
        if (isa<VarDecl>(D) && isTopLevelContext(CurrDeclContext) &&
            D->getDeclContext()->getParentSourceFile() ==
                CurrDeclContext->getParentSourceFile()) {
          return SemanticContextKind::Local;
        } else {
          return SemanticContextKind::CurrentModule;
        }
      } else {
        return SemanticContextKind::OtherModule;
      }

    case DeclVisibilityKind::DynamicLookup:
      // AnyObject results can come from different modules, including the
      // current module, but we always assign them the OtherModule semantic
      // context.  These declarations are uniqued by signature, so it is
      // totally random (determined by the hash function) which of the
      // equivalent declarations (across multiple modules) we will get.
      return SemanticContextKind::OtherModule;
    }
    llvm_unreachable("unhandled kind");
  }

  void addLeadingDot(CodeCompletionResultBuilder &Builder) {
    if (NeedOptionalUnwrap) {
      Builder.setNumBytesToErase(NumBytesToEraseForOptionalUnwrap);
      Builder.addQuestionMark();
      Builder.addLeadingDot();
      return;
    }
    if (needDot())
      Builder.addLeadingDot();
  }

  void addTypeAnnotation(CodeCompletionResultBuilder &Builder, Type T) {
    T = T->getReferenceStorageReferent();
    if (T->isVoid())
      Builder.addTypeAnnotation("Void");
    else
      Builder.addTypeAnnotation(T.getString());
  }

  void addTypeAnnotationForImplicitlyUnwrappedOptional(
      CodeCompletionResultBuilder &Builder, Type T,
      bool dynamicOrOptional = false) {

    std::string suffix;
    // FIXME: This retains previous behavior, but in reality the type of dynamic
    // lookups is IUO, not Optional as it is for the @optional attribute.
    if (dynamicOrOptional) {
      T = T->getOptionalObjectType();
      suffix = "?";
    }

    T = T->getReferenceStorageReferent();
    PrintOptions PO;
    PO.PrintOptionalAsImplicitlyUnwrapped = true;
    Builder.addTypeAnnotation(T.getString(PO) + suffix);
  }

  /// For printing in code completion results, replace archetypes with
  /// protocol compositions.
  ///
  /// FIXME: Perhaps this should be an option in PrintOptions instead.
  Type eraseArchetypes(ModuleDecl *M, Type type, GenericSignature *genericSig) {
    if (!genericSig)
      return type;

    auto buildProtocolComposition = [&](ArrayRef<ProtocolDecl *> protos) -> Type {
      SmallVector<Type, 2> types;
      for (auto proto : protos)
        types.push_back(proto->getDeclaredInterfaceType());
      return ProtocolCompositionType::get(M->getASTContext(), types,
                                          /*HasExplicitAnyObject=*/false);
    };

    if (auto *genericFuncType = type->getAs<GenericFunctionType>()) {
      SmallVector<AnyFunctionType::Param, 8> erasedParams;
      for (const auto &param : genericFuncType->getParams()) {
        auto erasedTy = eraseArchetypes(M, param.getPlainType(), genericSig);
        erasedParams.emplace_back(erasedTy, param.getLabel(),
                                  param.getParameterFlags());
      }
      return GenericFunctionType::get(genericSig,
          erasedParams,
          eraseArchetypes(M, genericFuncType->getResult(), genericSig),
          genericFuncType->getExtInfo());
    }

    return type.transform([&](Type t) -> Type {
      // FIXME: Code completion should only deal with one or the other,
      // and not both.
      if (auto *archetypeType = t->getAs<ArchetypeType>()) {
        auto protos = archetypeType->getConformsTo();
        if (!protos.empty())
          return buildProtocolComposition(protos);
      }

      if (t->isTypeParameter()) {
        auto protos = genericSig->getConformsTo(t);
        if (!protos.empty())
          return buildProtocolComposition(protos);
      }

      return t;
    });
  }

  Type getTypeOfMember(const ValueDecl *VD, Optional<Type> ExprType = None) {
    if (!ExprType)
      ExprType = this->ExprType;

    auto *M = CurrDeclContext->getParentModule();
    auto *GenericSig = VD->getInnermostDeclContext()
        ->getGenericSignatureOfContext();

    assert(VD->hasValidSignature());
    Type T = VD->getInterfaceType();

    if (*ExprType) {
      Type ContextTy = VD->getDeclContext()->getDeclaredInterfaceType();
      if (ContextTy) {
        // Look through lvalue types and metatypes
        Type MaybeNominalType = (*ExprType)->getRValueType();

        if (auto Metatype = MaybeNominalType->getAs<MetatypeType>())
          MaybeNominalType = Metatype->getInstanceType();

        if (auto SelfType = MaybeNominalType->getAs<DynamicSelfType>())
          MaybeNominalType = SelfType->getSelfType();

        // For optional protocol requirements and dynamic dispatch,
        // strip off optionality from the base type, but only if
        // we're not actually completing a member of Optional.
        if (!ContextTy->getOptionalObjectType() &&
            MaybeNominalType->getOptionalObjectType())
          MaybeNominalType = MaybeNominalType->getOptionalObjectType();

        // For dynamic lookup don't substitute in the base type.
        if (MaybeNominalType->isAnyObject())
          return T;

        // FIXME: Sometimes ExprType is the type of the member here,
        // and not the type of the base. That is inconsistent and
        // should be cleaned up.
        if (!MaybeNominalType->mayHaveMembers())
          return T;

        // For everything else, substitute in the base type.
        auto Subs = MaybeNominalType->getMemberSubstitutionMap(M, VD);

        // Pass in DesugarMemberTypes so that we see the actual
        // concrete type witnesses instead of type alias types.
        T = T.subst(Subs,
                    (SubstFlags::DesugarMemberTypes |
                     SubstFlags::UseErrorType));
      }
    }

    return eraseArchetypes(M, T, GenericSig);
  }

  Type getAssociatedTypeType(const AssociatedTypeDecl *ATD) {
    Type BaseTy = BaseType;
    if (!BaseTy)
      BaseTy = ExprType;
    if (!BaseTy && CurrDeclContext)
      BaseTy = CurrDeclContext->getInnermostTypeContext()
                   ->getDeclaredTypeInContext();
    if (BaseTy) {
      BaseTy = BaseTy->getInOutObjectType()->getMetatypeInstanceType();
      if (auto NTD = BaseTy->getAnyNominal()) {
        auto *Module = NTD->getParentModule();
        auto Conformance = Module->lookupConformance(
            BaseTy, ATD->getProtocol());
        if (Conformance && Conformance->isConcrete()) {
          return Conformance->getConcrete()
              ->getTypeWitness(const_cast<AssociatedTypeDecl *>(ATD),
                               nullptr);
        }
      }
    }
    return Type();
  }

  void addVarDeclRef(const VarDecl *VD, DeclVisibilityKind Reason) {
    if (!VD->hasName() ||
        !VD->isAccessibleFrom(CurrDeclContext) ||
        VD->shouldHideFromEditor())
      return;

    StringRef Name = VD->getName().get();
    assert(!Name.empty() && "name should not be empty");

    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(VD, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(VD);
    addLeadingDot(Builder);
    Builder.addTextChunk(Name);
    setClangDeclKeywords(VD, Pairs, Builder);

    if (!VD->hasValidSignature())
      return;

    // Add a type annotation.
    Type VarType = getTypeOfMember(VD);
    if (VD->getName() != Ctx.Id_self && VD->isInOut()) {
      // It is useful to show inout for function parameters.
      // But for 'self' it is just noise.
      VarType = InOutType::get(VarType);
    }
    auto DynamicOrOptional =
        IsDynamicLookup || VD->getAttrs().hasAttribute<OptionalAttr>();
    if (DynamicOrOptional) {
      // Values of properties that were found on a AnyObject have
      // Optional<T> type.  Same applies to optional members.
      VarType = OptionalType::get(VarType);
    }
    if (VD->getAttrs().hasAttribute<ImplicitlyUnwrappedOptionalAttr>())
      addTypeAnnotationForImplicitlyUnwrappedOptional(Builder, VarType,
                                                      DynamicOrOptional);
    else
      addTypeAnnotation(Builder, VarType);
  }

  void addParameters(CodeCompletionResultBuilder &Builder,
                     const ParameterList *params) {
    bool NeedComma = false;
    for (auto &param : *params) {
      if (NeedComma)
        Builder.addComma();
      NeedComma = true;

      Type type = param->getInterfaceType();
      if (param->isVariadic())
        type = ParamDecl::getVarargBaseTy(type);

      auto isIUO =
          param->getAttrs().hasAttribute<ImplicitlyUnwrappedOptionalAttr>();

      Builder.addCallParameter(param->getArgumentName(), type,
                               param->isVariadic(), /*Outermost*/ true,
                               param->isInOut(), isIUO, param->isAutoClosure());
    }
  }

  static bool hasInterestingDefaultValues(const AbstractFunctionDecl *func) {
    if (!func) return false;

    for (auto param : *func->getParameters()) {
      switch (param->getDefaultArgumentKind()) {
      case DefaultArgumentKind::Normal:
      case DefaultArgumentKind::Inherited: // FIXME: include this?
        return true;
      default:
        break;
      }
    }
    return false;
  }

  // Returns true if any content was added to Builder.
  bool addParamPatternFromFunction(CodeCompletionResultBuilder &Builder,
                                   const AnyFunctionType *AFT,
                                   const AbstractFunctionDecl *AFD,
                                   bool includeDefaultArgs = true) {

    const ParameterList *BodyParams = nullptr;
    const ParamDecl *SelfDecl = nullptr;

    if (AFD) {
      BodyParams = AFD->getParameters();

      // FIXME: Hack because we don't know which parameter list we're
      // actually working with.
      const unsigned expectedNumParams = AFT->getParams().size();
      if (expectedNumParams != BodyParams->size()) {
        BodyParams = nullptr;

        // Adjust to the "self" list if that is present, otherwise give up.
        if (expectedNumParams == 1 && AFD->getImplicitSelfDecl())
          SelfDecl = AFD->getImplicitSelfDecl();
        BodyParams = nullptr;
      }
    }

    bool modifiedBuilder = false;

    // Determine whether we should skip this argument because it is defaulted.
    auto shouldSkipArg = [&](unsigned i) -> bool {
      if (!BodyParams || i >= BodyParams->size())
        return false;

      switch (BodyParams->get(i)->getDefaultArgumentKind()) {
        case DefaultArgumentKind::None:
          return false;

        case DefaultArgumentKind::Normal:
        case DefaultArgumentKind::Inherited:
        case DefaultArgumentKind::NilLiteral:
        case DefaultArgumentKind::EmptyArray:
        case DefaultArgumentKind::EmptyDictionary:
          return !includeDefaultArgs;

        case DefaultArgumentKind::File:
        case DefaultArgumentKind::Line:
        case DefaultArgumentKind::Column:
        case DefaultArgumentKind::Function:
        case DefaultArgumentKind::DSOHandle:
          // Skip parameters that are defaulted to source location or other
          // caller context information.  Users typically don't want to specify
          // these parameters.
          return true;
      }

      llvm_unreachable("Unhandled DefaultArgumentKind in switch.");
    };

    bool NeedComma = false;
    // Iterate over each parameter.
    for (unsigned i = 0, e = AFT->getParams().size(); i != e; ++i) {
      // If we should skip this argument, do so.
      if (shouldSkipArg(i)) continue;

      const auto &Param = AFT->getParams()[i];
      auto ParamType = Param.isVariadic()
                     ? ParamDecl::getVarargBaseTy(Param.getPlainType())
                     : Param.getPlainType();

      if (NeedComma)
        Builder.addComma();
      if (BodyParams || SelfDecl) {
        auto *PD = (BodyParams ? BodyParams->get(i) : SelfDecl);

        // If we have a local name for the parameter, pass in that as well.
        auto argName = PD->getArgumentName();
        auto bodyName = PD->getName();
        auto isIUO =
            PD->getAttrs().hasAttribute<ImplicitlyUnwrappedOptionalAttr>();
        Builder.addCallParameter(argName, bodyName, ParamType,
                                 Param.isVariadic(), /*TopLevel*/ true,
                                 Param.isInOut(), isIUO, Param.isAutoClosure());
      } else {
        Builder.addCallParameter(
            Param.getLabel(), ParamType, Param.isVariadic(), /*TopLevel*/ true,
            Param.isInOut(), /*isIUO*/ false, Param.isAutoClosure());
      }
      modifiedBuilder = true;
      NeedComma = true;
    }

    return modifiedBuilder;
  }

  static void addThrows(CodeCompletionResultBuilder &Builder,
                        const AnyFunctionType *AFT,
                        const AbstractFunctionDecl *AFD) {
    if (AFD && AFD->getAttrs().hasAttribute<RethrowsAttr>())
      Builder.addAnnotatedRethrows();
    else if (AFT->throws())
      Builder.addAnnotatedThrows();
  }

  void addPoundAvailable(Optional<StmtKind> ParentKind) {
    if (ParentKind != StmtKind::If && ParentKind != StmtKind::Guard)
      return;
    CodeCompletionResultBuilder Builder(Sink, CodeCompletionResult::ResultKind::Keyword,
      SemanticContextKind::ExpressionSpecific, ExpectedTypes);
    Builder.addTextChunk("available");
    Builder.addLeftParen();
    Builder.addSimpleTypedParameter("Platform", /*IsVarArg=*/true);
    Builder.addComma();
    Builder.addTextChunk("*");
    Builder.addRightParen();
  }

  void addPoundSelector(bool needPound) {
    // #selector is only available when the Objective-C runtime is.
    if (!Ctx.LangOpts.EnableObjCInterop) return;

    // After #, this is a very likely result. When just in a String context,
    // it's not.
    auto semanticContext = needPound ? SemanticContextKind::None
    : SemanticContextKind::ExpressionSpecific;

    CodeCompletionResultBuilder Builder(
                                  Sink,
                                  CodeCompletionResult::ResultKind::Keyword,
                                  semanticContext, ExpectedTypes);
    if (needPound)
      Builder.addTextChunk("#selector");
    else
      Builder.addTextChunk("selector");
    Builder.addLeftParen();
    Builder.addSimpleTypedParameter("@objc method", /*IsVarArg=*/false);
    Builder.addRightParen();
  }

  void addPoundKeyPath(bool needPound) {
    // #keyPath is only available when the Objective-C runtime is.
    if (!Ctx.LangOpts.EnableObjCInterop) return;

    // After #, this is a very likely result. When just in a String context,
    // it's not.
    auto semanticContext = needPound ? SemanticContextKind::None
                                     : SemanticContextKind::ExpressionSpecific;

    CodeCompletionResultBuilder Builder(
                                  Sink,
                                  CodeCompletionResult::ResultKind::Keyword,
                                  semanticContext, ExpectedTypes);
    if (needPound)
      Builder.addTextChunk("#keyPath");
    else
      Builder.addTextChunk("keyPath");
    Builder.addLeftParen();
    Builder.addSimpleTypedParameter("@objc property sequence",
                                    /*IsVarArg=*/false);
    Builder.addRightParen();
  }

  void addFunctionCallPattern(const AnyFunctionType *AFT,
                              const AbstractFunctionDecl *AFD = nullptr) {
    if (AFD)
      foundFunction(AFD);
    else
      foundFunction(AFT);

    // Add the pattern, possibly including any default arguments.
    auto addPattern = [&](bool includeDefaultArgs = true) {
      // FIXME: to get the corect semantic context we need to know how lookup
      // would have found the declaration AFD. For now, just choose a reasonable
      // default, it's most likely to be CurrentModule or CurrentNominal.
      CodeCompletionResultBuilder Builder(
          Sink, CodeCompletionResult::ResultKind::Pattern,
          SemanticContextKind::CurrentModule, ExpectedTypes);
      if (!HaveLParen)
        Builder.addLeftParen();
      else
        Builder.addAnnotatedLeftParen();

      bool anyParam = addParamPatternFromFunction(Builder, AFT, AFD, includeDefaultArgs);

      if (HaveLParen && !anyParam) {
        // Empty result, don't add it.
        Builder.cancel();
        return;
      }

      // The rparen matches the lparen here so that we insert both or neither.
      if (!HaveLParen)
        Builder.addRightParen();
      else
        Builder.addAnnotatedRightParen();

      addThrows(Builder, AFT, AFD);

      if (AFD &&
          AFD->getAttrs().hasAttribute<ImplicitlyUnwrappedOptionalAttr>())
        addTypeAnnotationForImplicitlyUnwrappedOptional(Builder,
                                                        AFT->getResult());
      else
        addTypeAnnotation(Builder, AFT->getResult());
    };

    if (hasInterestingDefaultValues(AFD))
      addPattern(/*includeDefaultArgs*/ false);
    addPattern();
  }
  bool isImplicitlyCurriedInstanceMethod(const AbstractFunctionDecl *FD) {
    switch (Kind) {
    case LookupKind::ValueExpr:
      return ExprType->is<AnyMetatypeType>() && !FD->isStatic();
    case LookupKind::ValueInDeclContext:
      if (InsideStaticMethod &&
          FD->getDeclContext() == CurrentMethod->getDeclContext() &&
          !FD->isStatic())
        return true;
      if (auto Init = dyn_cast<Initializer>(CurrDeclContext))
        return FD->getDeclContext() == Init->getParent() && !FD->isStatic();
      return false;
    case LookupKind::EnumElement:
    case LookupKind::Type:
    case LookupKind::TypeInDeclContext:
      llvm_unreachable("cannot have a method call while doing a "
                       "type completion");
    case LookupKind::ImportFromModule:
      return false;
    }

    llvm_unreachable("Unhandled LookupKind in switch.");
  }

  void addMethodCall(const FuncDecl *FD, DeclVisibilityKind Reason) {
    if (FD->getName().empty())
      return;
    foundFunction(FD);
    bool IsImplicitlyCurriedInstanceMethod =
        isImplicitlyCurriedInstanceMethod(FD);

    StringRef Name = FD->getName().get();
    assert(!Name.empty() && "name should not be empty");

    Type FunctionType = getTypeOfMember(FD);
    assert(FunctionType);

    unsigned NumParamLists;
    if (FD->hasImplicitSelfDecl()) {
      if (IsImplicitlyCurriedInstanceMethod)
        NumParamLists = 2;
      else {
        NumParamLists = 1;

        // Strip off 'self'
        if (FunctionType->is<AnyFunctionType>())
          FunctionType = FunctionType->castTo<AnyFunctionType>()->getResult();
      }
    } else {
      NumParamLists = 1;
    }

    bool trivialTrailingClosure = false;
    if (!IsImplicitlyCurriedInstanceMethod &&
        FunctionType->is<AnyFunctionType>()) {
      trivialTrailingClosure = hasTrivialTrailingClosure(
          FD, FunctionType->castTo<AnyFunctionType>());
    }

    // Add the method, possibly including any default arguments.
    auto addMethodImpl = [&](bool includeDefaultArgs = true,
                             bool trivialTrailingClosure = false) {
      CommandWordsPairs Pairs;
      CodeCompletionResultBuilder Builder(
          Sink, CodeCompletionResult::ResultKind::Declaration,
          getSemanticContext(FD, Reason), ExpectedTypes);
      setClangDeclKeywords(FD, Pairs, Builder);
      Builder.setAssociatedDecl(FD);
      addLeadingDot(Builder);
      Builder.addTextChunk(Name);
      if (IsDynamicLookup)
        Builder.addDynamicLookupMethodCallTail();
      else if (FD->getAttrs().hasAttribute<OptionalAttr>())
        Builder.addOptionalMethodCallTail();

      llvm::SmallString<32> TypeStr;

      if (!FunctionType->is<AnyFunctionType>()) {
        llvm::raw_svector_ostream OS(TypeStr);
        FunctionType.print(OS);
        Builder.addTypeAnnotation(OS.str());
        return;
      }

      auto AFT = FunctionType->castTo<AnyFunctionType>();
      if (IsImplicitlyCurriedInstanceMethod) {
        Builder.addLeftParen();
        auto SelfParam = AFT->getParams()[0];
        Builder.addCallParameter(Ctx.Id_self, SelfParam.getPlainType(),
                                 /*IsVarArg*/ false, /*TopLevel*/ true,
                                 SelfParam.isInOut(),
                                 /*isIUO*/ false, /*isAutoClosure*/ false);
        Builder.addRightParen();
      } else if (trivialTrailingClosure) {
        Builder.addBraceStmtWithCursor(" { code }");
      } else {
        Builder.addLeftParen();
        addParamPatternFromFunction(Builder, AFT, FD, includeDefaultArgs);
        Builder.addRightParen();
        addThrows(Builder, AFT, FD);
      }

      Type ResultType = AFT->getResult();

      // Build type annotation.
      {
        llvm::raw_svector_ostream OS(TypeStr);
        for (unsigned i = 0; i < NumParamLists - 1; ++i) {
          ResultType->castTo<AnyFunctionType>()->printParams(OS);
          ResultType = ResultType->castTo<AnyFunctionType>()->getResult();
          OS << " -> ";
        }
        // What's left is the result type.
        if (ResultType->isVoid()) {
          OS << "Void";
        } else {
          // As we did with parameters in addParamPatternFromFunction,
          // for regular methods we'll print '!' after implicitly
          // unwrapped optional results.
          bool IsIUO =
              !IsImplicitlyCurriedInstanceMethod &&
              FD->getAttrs().hasAttribute<ImplicitlyUnwrappedOptionalAttr>();

          PrintOptions PO;
          PO.PrintOptionalAsImplicitlyUnwrapped = IsIUO;
          ResultType.print(OS, PO);
        }
      }
      Builder.addTypeAnnotation(TypeStr);
    };

    if (FunctionType->is<AnyFunctionType>() &&
        hasInterestingDefaultValues(FD)) {
      addMethodImpl(/*includeDefaultArgs*/ false);
    }
    if (trivialTrailingClosure) {
      addMethodImpl(/*includeDefaultArgs=*/false,
                    /*trivialTrailingClosure=*/true);
    }
    addMethodImpl();
  }

  void addConstructorCall(const ConstructorDecl *CD, DeclVisibilityKind Reason,
                          Optional<Type> BaseType, Optional<Type> Result,
                          bool IsOnType = true,
                          Identifier addName = Identifier()) {
    foundFunction(CD);
    Type MemberType = getTypeOfMember(CD, BaseType);
    AnyFunctionType *ConstructorType = nullptr;
    if (auto MemberFuncType = MemberType->getAs<AnyFunctionType>())
      ConstructorType = MemberFuncType->getResult()
                                      ->castTo<AnyFunctionType>();

    bool needInit = false;
    if (!IsOnType) {
      assert(addName.empty());
      needInit = true;
    } else if (addName.empty() && HaveDot) {
      needInit = true;
    }

    // If we won't be able to provide a result, bail out.
    if (MemberType->hasError() && addName.empty() && !needInit)
      return;

    // Add the constructor, possibly including any default arguments.
    auto addConstructorImpl = [&](bool includeDefaultArgs = true) {
      CommandWordsPairs Pairs;
      CodeCompletionResultBuilder Builder(
          Sink, CodeCompletionResult::ResultKind::Declaration,
          getSemanticContext(CD, Reason), ExpectedTypes);
      setClangDeclKeywords(CD, Pairs, Builder);
      Builder.setAssociatedDecl(CD);
      if (needInit) {
        assert(addName.empty());
        addLeadingDot(Builder);
        Builder.addTextChunk("init");
      } else if (!addName.empty()) {
        Builder.addTextChunk(addName.str());
      } else {
        assert(!MemberType->hasError() && "will insert empty result");
      }

      if (!ConstructorType) {
        addTypeAnnotation(Builder, MemberType);
        return;
      }

      if (!HaveLParen)
        Builder.addLeftParen();
      else
        Builder.addAnnotatedLeftParen();

      bool anyParam = addParamPatternFromFunction(Builder, ConstructorType, CD,
                                  includeDefaultArgs);

      if (HaveLParen && !anyParam) {
        // Empty result, don't add it.
        Builder.cancel();
        return;
      }

      // The rparen matches the lparen here so that we insert both or neither.
      if (!HaveLParen)
        Builder.addRightParen();
      else
        Builder.addAnnotatedRightParen();

      addThrows(Builder, ConstructorType, CD);

      if (CD->getAttrs().hasAttribute<ImplicitlyUnwrappedOptionalAttr>()) {
        addTypeAnnotationForImplicitlyUnwrappedOptional(
            Builder, Result.hasValue() ? Result.getValue()
                                       : ConstructorType->getResult());
      } else {
        addTypeAnnotation(Builder, Result.hasValue()
                                       ? Result.getValue()
                                       : ConstructorType->getResult());
      }
    };

    if (ConstructorType && hasInterestingDefaultValues(CD))
      addConstructorImpl(/*includeDefaultArgs*/ false);
    addConstructorImpl();
  }

  void addConstructorCallsForType(Type type, Identifier name,
                                  DeclVisibilityKind Reason) {
    if (!Ctx.LangOpts.CodeCompleteInitsInPostfixExpr && !IsUnresolvedMember)
      return;

    assert(CurrDeclContext);
    SmallVector<ValueDecl *, 16> initializers;
    if (CurrDeclContext->lookupQualified(type, DeclBaseName::createConstructor(),
                                         NL_QualifiedDefault,
                                         TypeResolver, initializers)) {
      for (auto *init : initializers) {
        if (init->shouldHideFromEditor())
          continue;
        if (IsUnresolvedMember &&
            cast<ConstructorDecl>(init)->getFailability() == OTK_Optional) {
          continue;
        }
        addConstructorCall(cast<ConstructorDecl>(init), Reason, type, None,
                           /*IsOnType=*/true, name);
      }
    }
  }

  void addSubscriptCall(const SubscriptDecl *SD, DeclVisibilityKind Reason) {
    // Don't add subscript call to meta types.
    if (!ExprType || ExprType->is<AnyMetatypeType>())
      return;

    // Subscript after '.' is valid only after type part of Swift keypath
    // expression. (e.g. '\TyName.SubTy.[0])
    if (HaveDot && !IsAfterSwiftKeyPathRoot)
      return;

    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(SD, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(SD);
    setClangDeclKeywords(SD, Pairs, Builder);

    // '\TyName#^TOKEN^#' requires leading dot.
    if (!HaveDot && IsAfterSwiftKeyPathRoot)
      Builder.addLeadingDot();

    if (NeedOptionalUnwrap) {
      Builder.setNumBytesToErase(NumBytesToEraseForOptionalUnwrap);
      Builder.addQuestionMark();
    }

    Builder.addLeftBracket();
    addParameters(Builder, SD->getIndices());
    Builder.addRightBracket();

    // Add a type annotation.
    Type T = SD->getElementInterfaceType();
    if (IsDynamicLookup) {
      // Values of properties that were found on a AnyObject have
      // Optional<T> type.
      T = OptionalType::get(T);
    }
    addTypeAnnotation(Builder, T);
  }

  void addNominalTypeRef(const NominalTypeDecl *NTD,
                         DeclVisibilityKind Reason) {
    if (IsUnresolvedMember)
      return;
    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(NTD, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(NTD);
    setClangDeclKeywords(NTD, Pairs, Builder);
    addLeadingDot(Builder);
    Builder.addTextChunk(NTD->getName().str());
    addTypeAnnotation(Builder, NTD->getDeclaredType());
  }

  void addTypeAliasRef(const TypeAliasDecl *TAD, DeclVisibilityKind Reason) {
    if (IsUnresolvedMember)
      return;
    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(TAD, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(TAD);
    setClangDeclKeywords(TAD, Pairs, Builder);
    addLeadingDot(Builder);
    Builder.addTextChunk(TAD->getName().str());
    if (TAD->hasInterfaceType()) {
      auto underlyingType = TAD->getUnderlyingTypeLoc().getType();
      if (underlyingType->hasError()) {
        Type parentType;
        if (auto nominal = TAD->getDeclContext()->getSelfNominalTypeDecl()) {
          parentType = nominal->getDeclaredInterfaceType();
        }
        addTypeAnnotation(
                      Builder,
                      NameAliasType::get(const_cast<TypeAliasDecl *>(TAD),
                                         parentType, SubstitutionMap(),
                                         underlyingType));

      } else {
        addTypeAnnotation(Builder, underlyingType);
      }
    }
  }

  void addGenericTypeParamRef(const GenericTypeParamDecl *GP,
                              DeclVisibilityKind Reason) {
    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(GP, Reason), ExpectedTypes);
    setClangDeclKeywords(GP, Pairs, Builder);
    Builder.setAssociatedDecl(GP);
    addLeadingDot(Builder);
    Builder.addTextChunk(GP->getName().str());
    addTypeAnnotation(Builder, GP->getDeclaredInterfaceType());
  }

  void addAssociatedTypeRef(const AssociatedTypeDecl *AT,
                            DeclVisibilityKind Reason) {
    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(AT, Reason), ExpectedTypes);
    setClangDeclKeywords(AT, Pairs, Builder);
    Builder.setAssociatedDecl(AT);
    addLeadingDot(Builder);
    Builder.addTextChunk(AT->getName().str());
    if (Type T = getAssociatedTypeType(AT))
      addTypeAnnotation(Builder, T);
  }

  void addPrecedenceGroupRef(PrecedenceGroupDecl *PGD) {
    auto semanticContext =
      getSemanticContext(PGD, DeclVisibilityKind::VisibleAtTopLevel);
    CodeCompletionResultBuilder builder(
      Sink, CodeCompletionResult::ResultKind::Declaration,
      semanticContext, {});

    builder.addTextChunk(PGD->getName().str());
    builder.setAssociatedDecl(PGD);
  };

  void addEnumElementRef(const EnumElementDecl *EED,
                         DeclVisibilityKind Reason,
                         bool HasTypeContext) {
    if (!EED->hasName() ||
        !EED->isAccessibleFrom(CurrDeclContext) ||
        EED->shouldHideFromEditor())
      return;

    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        HasTypeContext ? SemanticContextKind::ExpressionSpecific
                       : getSemanticContext(EED, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(EED);
    setClangDeclKeywords(EED, Pairs, Builder);
    addLeadingDot(Builder);
    Builder.addTextChunk(EED->getName().str());
    if (auto *params = EED->getParameterList()) {
      Builder.addLeftParen();
      addParameters(Builder, params);
      Builder.addRightParen();
    }

    // Enum element is of function type such as EnumName.type -> Int ->
    // EnumName; however we should show Int -> EnumName as the type
    Type EnumType;
    if (EED->hasInterfaceType()) {
      EnumType = EED->getInterfaceType();
      if (auto FuncType = EnumType->getAs<AnyFunctionType>()) {
        EnumType = FuncType->getResult();
      }
    }
    if (EnumType)
      addTypeAnnotation(Builder, EnumType);
  }

  void addKeyword(StringRef Name, Type TypeAnnotation = Type(),
                  SemanticContextKind SK = SemanticContextKind::None,
                  CodeCompletionKeywordKind KeyKind
                    = CodeCompletionKeywordKind::None) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword, SK, ExpectedTypes);
    addLeadingDot(Builder);
    Builder.addTextChunk(Name);
    Builder.setKeywordKind(KeyKind);
    if (TypeAnnotation)
      addTypeAnnotation(Builder, TypeAnnotation);
  }

  void addKeyword(StringRef Name, StringRef TypeAnnotation,
                  CodeCompletionKeywordKind KeyKind
                    = CodeCompletionKeywordKind::None) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, ExpectedTypes);
    addLeadingDot(Builder);
    Builder.addTextChunk(Name);
    Builder.setKeywordKind(KeyKind);
    if (!TypeAnnotation.empty())
      Builder.addTypeAnnotation(TypeAnnotation);
  }

  void addDeclAttrParamKeyword(StringRef Name, StringRef Annotation,
                             bool NeedSpecify) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, ExpectedTypes);
    Builder.addDeclAttrParamKeyword(Name, Annotation, NeedSpecify);
  }

  void addDeclAttrKeyword(StringRef Name, StringRef Annotation) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, ExpectedTypes);
    Builder.addDeclAttrKeyword(Name, Annotation);
  }

  /// Add the compound function name for the given function.
  void addCompoundFunctionName(AbstractFunctionDecl *AFD,
                               DeclVisibilityKind Reason) {
    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(AFD, Reason), ExpectedTypes);
    setClangDeclKeywords(AFD, Pairs, Builder);
    Builder.setAssociatedDecl(AFD);

    // Base name
    addLeadingDot(Builder);
    Builder.addTextChunk(AFD->getBaseName().userFacingName());

    // Add the argument labels.
    auto ArgLabels = AFD->getFullName().getArgumentNames();
    if (!ArgLabels.empty()) {
      if (!HaveLParen)
        Builder.addLeftParen();
      else
        Builder.addAnnotatedLeftParen();

      for (auto ArgLabel : ArgLabels) {
        if (ArgLabel.empty())
          Builder.addTextChunk("_");
        else
          Builder.addTextChunk(ArgLabel.str());
        Builder.addTextChunk(":");
      }

      Builder.addRightParen();
    }
  }

  // Implement swift::VisibleDeclConsumer.
  void foundDecl(ValueDecl *D, DeclVisibilityKind Reason) override {
    if (D->shouldHideFromEditor())
      return;

    if (IsKeyPathExpr && !KeyPathFilter(D, Reason))
      return;

    if (IsSwiftKeyPathExpr && !SwiftKeyPathFilter(D, Reason))
      return;
        
    if (!D->hasInterfaceType())
      TypeResolver->resolveDeclSignature(D);
    else if (isa<TypeAliasDecl>(D)) {
      // A TypeAliasDecl might have type set, but not the underlying type.
      TypeResolver->resolveDeclSignature(D);
    }

    switch (Kind) {
    case LookupKind::ValueExpr:
      if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
        // Do we want compound function names here?
        if (shouldUseFunctionReference(CD)) {
          addCompoundFunctionName(CD, Reason);
          return;
        }

        if (auto MT = ExprType->getAs<AnyMetatypeType>()) {
          Type Ty = MT->getInstanceType();
          assert(Ty && "Cannot find instance type.");

          // If instance type is type alias, show users that the constructed
          // type is the typealias instead of the underlying type of the alias.
          Optional<Type> Result = None;
          if (!CD->getInterfaceType()->is<ErrorType>() &&
              isa<NameAliasType>(Ty.getPointer()) &&
              Ty->getDesugaredType() ==
                CD->getResultInterfaceType().getPointer()) {
            Result = Ty;
          }
          // If the expression type is not a static metatype or an archetype, the base
          // is not a type. Direct call syntax is illegal on values, so we only add
          // initializer completions if we do not have a left parenthesis and either
          // the initializer is required, the base type's instance type is not a class,
          // or this is a 'self' or 'super' reference.
          if (IsStaticMetatype || IsUnresolvedMember || Ty->is<ArchetypeType>())
            addConstructorCall(CD, Reason, None, Result, /*isOnType*/true);
          else if ((IsSelfRefExpr || IsSuperRefExpr || !Ty->is<ClassType>() ||
                    CD->isRequired()) && !HaveLParen)
            addConstructorCall(CD, Reason, None, Result, /*isOnType*/false);
          return;
        }
        if (!HaveLParen) {
          auto CDC = dyn_cast<ConstructorDecl>(CurrDeclContext);
          if (!CDC)
            return;
          // We do not want 'init' completions for 'self' in non-convenience
          // initializers and for 'super' in convenience initializers.
          if ((IsSelfRefExpr && CDC->isConvenienceInit()) ||
              ((IsSuperRefExpr && !CDC->isConvenienceInit())))
            addConstructorCall(CD, Reason, None, None, /*IsOnType=*/false);
        }
        return;
      }

      if (HaveLParen)
        return;

      LLVM_FALLTHROUGH;

    case LookupKind::ValueInDeclContext:
    case LookupKind::ImportFromModule:
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        addVarDeclRef(VD, Reason);
        return;
      }

      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        // We cannot call operators with a postfix parenthesis syntax.
        if (FD->isBinaryOperator() || FD->isUnaryOperator())
          return;

        // We cannot call accessors.  We use VarDecls and SubscriptDecls to
        // produce completions that refer to getters and setters.
        if (isa<AccessorDecl>(FD))
          return;

        // Do we want compound function names here?
        if (shouldUseFunctionReference(FD)) {
          addCompoundFunctionName(FD, Reason);
          return;
        }

        addMethodCall(FD, Reason);
        return;
      }

      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addNominalTypeRef(NTD, Reason);
        addConstructorCallsForType(NTD->getDeclaredInterfaceType(),
                                   NTD->getName(), Reason);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addTypeAliasRef(TAD, Reason);
        auto type = TAD->mapTypeIntoContext(TAD->getDeclaredInterfaceType());
        if (type->mayHaveMembers())
          addConstructorCallsForType(type, TAD->getName(), Reason);
        return;
      }

      if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
        addGenericTypeParamRef(GP, Reason);
        for (auto *protocol : GP->getConformingProtocols())
          addConstructorCallsForType(protocol->getDeclaredInterfaceType(),
                                     GP->getName(), Reason);
        return;
      }

      if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
        addAssociatedTypeRef(AT, Reason);
        return;
      }

      if (auto *EED = dyn_cast<EnumElementDecl>(D)) {
        addEnumElementRef(EED, Reason, /*HasTypeContext=*/false);
        return;
      }

      // Swift key path allows .[0]
      if (auto *SD = dyn_cast<SubscriptDecl>(D)) {
        addSubscriptCall(SD, Reason);
        return;
      }
      return;

    case LookupKind::EnumElement:
      handleEnumElement(D, Reason);
      return;

    case LookupKind::Type:
    case LookupKind::TypeInDeclContext:
      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addNominalTypeRef(NTD, Reason);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addTypeAliasRef(TAD, Reason);
        return;
      }

      if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
        addGenericTypeParamRef(GP, Reason);
        return;
      }

      if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
        addAssociatedTypeRef(AT, Reason);
        return;
      }

      return;
    }
  }

  bool handleEnumElement(ValueDecl *D, DeclVisibilityKind Reason) {
    if (!D->hasInterfaceType())
      TypeResolver->resolveDeclSignature(D);

    if (auto *EED = dyn_cast<EnumElementDecl>(D)) {
      addEnumElementRef(EED, Reason, /*HasTypeContext=*/true);
      return true;
    } else if (auto *ED = dyn_cast<EnumDecl>(D)) {
      llvm::DenseSet<EnumElementDecl *> Elements;
      ED->getAllElements(Elements);
      for (auto *Ele : Elements) {
        if (!Ele->hasInterfaceType())
          TypeResolver->resolveDeclSignature(Ele);
        addEnumElementRef(Ele, Reason, /*HasTypeContext=*/true);
      }
      return true;
    }
    return false;
  }

  bool tryTupleExprCompletions(Type ExprType) {
    auto *TT = ExprType->getAs<TupleType>();
    if (!TT)
      return false;

    unsigned Index = 0;
    for (auto TupleElt : TT->getElements()) {
      CodeCompletionResultBuilder Builder(
          Sink,
          CodeCompletionResult::ResultKind::Pattern,
          SemanticContextKind::CurrentNominal, ExpectedTypes);
      addLeadingDot(Builder);
      if (TupleElt.hasName()) {
        Builder.addTextChunk(TupleElt.getName().str());
      } else {
        llvm::SmallString<4> IndexStr;
        {
          llvm::raw_svector_ostream OS(IndexStr);
          OS << Index;
        }
        Builder.addTextChunk(IndexStr.str());
      }
      addTypeAnnotation(Builder, TupleElt.getType());
      Index++;
    }
    return true;
  }

  bool tryFunctionCallCompletions(Type ExprType, const ValueDecl *VD) {
    ExprType = ExprType->getRValueType();
    if (auto AFT = ExprType->getAs<AnyFunctionType>()) {
      if (auto *AFD = dyn_cast_or_null<AbstractFunctionDecl>(VD)) {
        addFunctionCallPattern(AFT, AFD);
      } else {
        addFunctionCallPattern(AFT);
      }
      return true;
    }
    return false;
  }

  bool tryModuleCompletions(Type ExprType) {
    if (auto MT = ExprType->getAs<ModuleType>()) {
      ModuleDecl *M = MT->getModule();
      if (CurrDeclContext->getParentModule() != M) {
        // Only use the cache if it is not the current module.
        RequestedCachedResults.push_back(
          RequestedResultsTy::fromModule(M).needLeadingDot(needDot()));
        return true;
      }
    }
    return false;
  }

  /// If the given ExprType is optional, this adds completions for the unwrapped
  /// type.
  ///
  /// \return true if the given type was Optional .
  bool tryUnwrappedCompletions(Type ExprType, bool isIUO) {
    // FIXME: consider types convertible to T?.

    ExprType = ExprType->getRValueType();
    // FIXME: We don't always pass down whether a type is from an
    // unforced IUO.
    if (isIUO) {
      if (Type Unwrapped = ExprType->getOptionalObjectType()) {
        lookupVisibleMemberDecls(*this, Unwrapped, CurrDeclContext,
                                 TypeResolver, IncludeInstanceMembers);
        return true;
      }
      assert(IsUnwrappedOptional && "IUOs should be optional if not bound/forced");
      return false;
    }

    if (Type Unwrapped = ExprType->getOptionalObjectType()) {
      llvm::SaveAndRestore<bool> ChangeNeedOptionalUnwrap(NeedOptionalUnwrap,
                                                          true);
      if (DotLoc.isValid()) {
        NumBytesToEraseForOptionalUnwrap = Ctx.SourceMgr.getByteDistance(
            DotLoc, Ctx.SourceMgr.getCodeCompletionLoc());
      } else {
        NumBytesToEraseForOptionalUnwrap = 0;
      }
      if (NumBytesToEraseForOptionalUnwrap <=
          CodeCompletionResult::MaxNumBytesToErase) {
        if (!tryTupleExprCompletions(Unwrapped)) {
          lookupVisibleMemberDecls(*this, Unwrapped, CurrDeclContext,
                                   TypeResolver,
                                   IncludeInstanceMembers);
        }
      }
      return true;
    }
    return false;
  }

  void getValueExprCompletions(Type ExprType, ValueDecl *VD = nullptr) {
    Kind = LookupKind::ValueExpr;
    NeedLeadingDot = !HaveDot;

    ExprType = ExprType->getRValueType();
    assert(!ExprType->hasTypeParameter());

    this->ExprType = ExprType;

    // Open existential types, so that lookupVisibleMemberDecls() can properly
    // substitute them.
    bool WasOptional = false;
    if (auto OptionalType = ExprType->getOptionalObjectType()) {
      ExprType = OptionalType;
      WasOptional = true;
    }

    if (!ExprType->getMetatypeInstanceType()->isAnyObject())
      if (ExprType->isAnyExistentialType())
        ExprType = OpenedArchetypeType::getAny(ExprType);

    if (WasOptional)
      ExprType = OptionalType::get(ExprType);

    // Handle special cases
    bool isIUO = VD && VD->getAttrs()
        .hasAttribute<ImplicitlyUnwrappedOptionalAttr>();
    if (tryFunctionCallCompletions(ExprType, VD))
      return;
    if (tryModuleCompletions(ExprType))
      return;
    if (tryTupleExprCompletions(ExprType))
      return;
    // Don't check/return so we still add the members of Optional itself below
    tryUnwrappedCompletions(ExprType, isIUO);

    lookupVisibleMemberDecls(*this, ExprType, CurrDeclContext,
                             TypeResolver, IncludeInstanceMembers);
  }

  template <typename T>
  void collectOperatorsFromMap(SourceFile::OperatorMap<T> &map,
                               bool includePrivate,
                               std::vector<OperatorDecl *> &results) {
    for (auto &pair : map) {
      if (pair.second.getPointer() &&
          (pair.second.getInt() || includePrivate)) {
        results.push_back(pair.second.getPointer());
      }
    }
  }

  void collectOperatorsFrom(SourceFile *SF,
                            std::vector<OperatorDecl *> &results) {
    bool includePrivate = CurrDeclContext->getParentSourceFile() == SF;
    collectOperatorsFromMap(SF->PrefixOperators, includePrivate, results);
    collectOperatorsFromMap(SF->PostfixOperators, includePrivate, results);
    collectOperatorsFromMap(SF->InfixOperators, includePrivate, results);
  }

  void collectOperatorsFrom(LoadedFile *F,
                            std::vector<OperatorDecl *> &results) {
    SmallVector<Decl *, 64> topLevelDecls;
    F->getTopLevelDecls(topLevelDecls);
    for (auto D : topLevelDecls) {
      if (auto op = dyn_cast<OperatorDecl>(D))
        results.push_back(op);
    }
  }

  std::vector<OperatorDecl *> collectOperators() {
    std::vector<OperatorDecl *> results;
    assert(CurrDeclContext);
    CurrDeclContext->getParentSourceFile()->forAllVisibleModules(
    [&](ModuleDecl::ImportedModule import) {
      for (auto fileUnit : import.second->getFiles()) {
        switch (fileUnit->getKind()) {
        case FileUnitKind::Builtin:
        case FileUnitKind::ClangModule:
        case FileUnitKind::DWARFModule:
          continue;
        case FileUnitKind::Source:
          collectOperatorsFrom(cast<SourceFile>(fileUnit), results);
          break;
        case FileUnitKind::SerializedAST:
          collectOperatorsFrom(cast<LoadedFile>(fileUnit), results);
          break;
        }
      }
    });
    return results;
  }

  void addPostfixBang(Type resultType) {
    CodeCompletionResultBuilder builder(
        Sink, CodeCompletionResult::ResultKind::BuiltinOperator,
        SemanticContextKind::None, {});
    // FIXME: we can't use the exclamation mark chunk kind, or it isn't
    // included in the completion name.
    builder.addTextChunk("!");
    assert(resultType);
    addTypeAnnotation(builder, resultType);
  }

  void addPostfixOperatorCompletion(OperatorDecl *op, Type resultType) {
    // FIXME: we should get the semantic context of the function, not the
    // operator decl.
    auto semanticContext =
        getSemanticContext(op, DeclVisibilityKind::VisibleAtTopLevel);
    CodeCompletionResultBuilder builder(
        Sink, CodeCompletionResult::ResultKind::Declaration, semanticContext,
        {});

    // FIXME: handle variable amounts of space.
    if (HaveLeadingSpace)
      builder.setNumBytesToErase(1);
    builder.setAssociatedDecl(op);
    builder.addTextChunk(op->getName().str());
    assert(resultType);
    addTypeAnnotation(builder, resultType);
  }

  void tryPostfixOperator(Expr *expr, PostfixOperatorDecl *op) {
    ConcreteDeclRef referencedDecl;
    FunctionType *funcTy = getTypeOfCompletionOperator(
        const_cast<DeclContext *>(CurrDeclContext), expr, op->getName(),
        DeclRefKind::PostfixOperator, referencedDecl);
    if (!funcTy)
      return;

    // TODO: Use referencedDecl (FuncDecl) instead of 'op' (OperatorDecl).
    addPostfixOperatorCompletion(op, funcTy->getResult());
  }

  void addAssignmentOperator(Type RHSType, Type resultType) {
    CodeCompletionResultBuilder builder(
        Sink, CodeCompletionResult::ResultKind::BuiltinOperator,
        SemanticContextKind::None, {});

    if (HaveLeadingSpace)
      builder.addAnnotatedWhitespace(" ");
    else
      builder.addWhitespace(" ");
    builder.addEqual();
    builder.addWhitespace(" ");
    assert(RHSType && resultType);
    builder.addCallParameter(Identifier(), Identifier(), RHSType,
                             /*IsVarArg*/ false, /*TopLevel*/ true,
                             /*IsInOut*/ false, /*isIUO*/ false,
                             /*isAutoClosure*/ false);
    addTypeAnnotation(builder, resultType);
  }

  void addInfixOperatorCompletion(OperatorDecl *op, Type resultType,
                                  Type RHSType) {
    // FIXME: we should get the semantic context of the function, not the
    // operator decl.
    auto semanticContext =
        getSemanticContext(op, DeclVisibilityKind::VisibleAtTopLevel);
    CodeCompletionResultBuilder builder(
        Sink, CodeCompletionResult::ResultKind::Declaration, semanticContext,
        {});
    builder.setAssociatedDecl(op);

    if (HaveLeadingSpace)
      builder.addAnnotatedWhitespace(" ");
    else
      builder.addWhitespace(" ");
    builder.addTextChunk(op->getName().str());
    builder.addWhitespace(" ");
    if (RHSType)
      builder.addCallParameter(Identifier(), Identifier(), RHSType, false, true,
                               /*IsInOut*/ false, /*isIUO*/ false,
                               /*isAutoClosure*/ false);
    if (resultType)
      addTypeAnnotation(builder, resultType);
  }

  void tryInfixOperatorCompletion(Expr *foldedExpr, InfixOperatorDecl *op) {
    ConcreteDeclRef referencedDecl;
    FunctionType *funcTy = getTypeOfCompletionOperator(
        const_cast<DeclContext *>(CurrDeclContext), foldedExpr, op->getName(),
        DeclRefKind::BinaryOperator, referencedDecl);
    if (!funcTy)
      return;

    Type lhsTy = funcTy->getParams()[0].getPlainType();
    Type rhsTy = funcTy->getParams()[1].getPlainType();
    Type resultTy = funcTy->getResult();

    // Don't complete optional operators on non-optional types.
    if (!lhsTy->getRValueType()->getOptionalObjectType()) {
      // 'T ?? T'
      if (op->getName().str() == "??")
        return;
      // 'T == nil'
      if (auto NT = rhsTy->getNominalOrBoundGenericNominal())
        if (NT->getName() ==
            CurrDeclContext->getASTContext().Id_OptionalNilComparisonType)
          return;
    }

    // If the right-hand side and result type are both type parameters, we're
    // not providing a useful completion.
    if (resultTy->isTypeParameter() && rhsTy->isTypeParameter())
      return;

    // TODO: Use referencedDecl (FuncDecl) instead of 'op' (OperatorDecl).
    addInfixOperatorCompletion(op, funcTy->getResult(),
                               funcTy->getParams()[1].getPlainType());
  }

  Expr *typeCheckLeadingSequence(Expr *LHS, ArrayRef<Expr *> leadingSequence) {
    if (leadingSequence.empty())
      return LHS;

    assert(leadingSequence.size() % 2 == 0);
    SmallVector<Expr *, 3> sequence(leadingSequence.begin(),
                                    leadingSequence.end());
    sequence.push_back(LHS);

    Expr *expr =
        SequenceExpr::create(CurrDeclContext->getASTContext(), sequence);
    prepareForRetypechecking(expr);
    if (!typeCheckExpression(const_cast<DeclContext *>(CurrDeclContext),
                             expr)) {
      return expr;
    }
    return LHS;
  }

  void getOperatorCompletions(Expr *LHS, ArrayRef<Expr *> leadingSequence) {
    Expr *foldedExpr = typeCheckLeadingSequence(LHS, leadingSequence);

    std::vector<OperatorDecl *> operators = collectOperators();
    // FIXME: this always chooses the first operator with the given name.
    llvm::DenseSet<Identifier> seenPostfixOperators;
    llvm::DenseSet<Identifier> seenInfixOperators;

    for (auto op : operators) {
      switch (op->getKind()) {
      case DeclKind::PrefixOperator:
        // Don't insert prefix operators in postfix position.
        // FIXME: where should these get completed?
        break;
      case DeclKind::PostfixOperator:
        if (seenPostfixOperators.insert(op->getName()).second)
          tryPostfixOperator(LHS, cast<PostfixOperatorDecl>(op));
        break;
      case DeclKind::InfixOperator:
        if (seenInfixOperators.insert(op->getName()).second)
          tryInfixOperatorCompletion(foldedExpr, cast<InfixOperatorDecl>(op));
        break;
      default:
        llvm_unreachable("unexpected operator kind");
      }
    }

    if (leadingSequence.empty() && LHS->getType() &&
        LHS->getType()->hasLValueType()) {
      addAssignmentOperator(LHS->getType()->getRValueType(),
                            CurrDeclContext->getASTContext().TheEmptyTupleType);
    }

    // FIXME: unify this with the ?.member completions.
    if (auto T = LHS->getType())
      if (auto ValueT = T->getRValueType()->getOptionalObjectType())
        addPostfixBang(ValueT);
  }

  void addValueLiteralCompletions() {
    auto &context = CurrDeclContext->getASTContext();
    auto *module = CurrDeclContext->getParentModule();

    auto addFromProto = [&](
        CodeCompletionLiteralKind kind, StringRef defaultTypeName,
        llvm::function_ref<void(CodeCompletionResultBuilder &)> consumer,
        bool isKeyword = false) {

      CodeCompletionResultBuilder builder(Sink, CodeCompletionResult::Literal,
                                          SemanticContextKind::None, {});
      builder.setLiteralKind(kind);

      consumer(builder);

      // Check for matching ExpectedTypes.
      auto *P = context.getProtocol(protocolForLiteralKind(kind));
      bool foundConformance = false;
      for (auto T : ExpectedTypes) {
        if (!T)
          continue;

        auto typeRelation = CodeCompletionResult::Identical;
        // Convert through optional types unless we're looking for a protocol
        // that Optional itself conforms to.
        if (kind != CodeCompletionLiteralKind::NilLiteral) {
          if (auto optionalObjT = T->getOptionalObjectType()) {
            T = optionalObjT;
            typeRelation = CodeCompletionResult::Convertible;
          }
        }

        // Check for conformance to the literal protocol.
        if (auto *NTD = T->getAnyNominal()) {
          SmallVector<ProtocolConformance *, 2> conformances;
          if (NTD->lookupConformance(module, P, conformances)) {
            foundConformance = true;
            addTypeAnnotation(builder, T);
            builder.setExpectedTypeRelation(typeRelation);
          }
        }
      }

      // Fallback to showing the default type.
      if (!foundConformance && !defaultTypeName.empty())
        builder.addTypeAnnotation(defaultTypeName);
    };

    // FIXME: the pedantically correct way is to resolve Swift.*LiteralType.

    using LK = CodeCompletionLiteralKind;
    using Builder = CodeCompletionResultBuilder;

    // Add literal completions that conform to specific protocols.
    addFromProto(LK::IntegerLiteral, "Int", [](Builder &builder) {
      builder.addTextChunk("0");
    });
    addFromProto(LK::BooleanLiteral, "Bool", [](Builder &builder) {
      builder.addTextChunk("true");
    }, /*isKeyword=*/true);
    addFromProto(LK::BooleanLiteral, "Bool", [](Builder &builder) {
      builder.addTextChunk("false");
    }, /*isKeyword=*/true);
    addFromProto(LK::NilLiteral, "", [](Builder &builder) {
      builder.addTextChunk("nil");
    }, /*isKeyword=*/true);
    addFromProto(LK::StringLiteral, "String", [&](Builder &builder) {
      builder.addTextChunk("\"");
      builder.addSimpleNamedParameter("abc");
      builder.addTextChunk("\"");
    });
    addFromProto(LK::ArrayLiteral, "Array", [&](Builder &builder) {
      builder.addLeftBracket();
      builder.addSimpleNamedParameter("values");
      builder.addRightBracket();
    });
    addFromProto(LK::DictionaryLiteral, "Dictionary", [&](Builder &builder) {
      builder.addLeftBracket();
      builder.addSimpleNamedParameter("key");
      builder.addTextChunk(": ");
      builder.addSimpleNamedParameter("value");
      builder.addRightBracket();
    });

    auto floatType = context.getFloatDecl()->getDeclaredType();
    addFromProto(LK::ColorLiteral, "", [&](Builder &builder) {
      builder.addTextChunk("#colorLiteral");
      builder.addLeftParen();
      builder.addCallParameter(context.getIdentifier("red"), floatType, false,
                               true, /*IsInOut*/ false,
                               /*isIUO*/ false, /*isAutoClosure*/ false);
      builder.addComma();
      builder.addCallParameter(context.getIdentifier("green"), floatType, false,
                               true, /*IsInOut*/ false, /*isIUO*/ false,
                               /*isAutoClosure*/ false);
      builder.addComma();
      builder.addCallParameter(context.getIdentifier("blue"), floatType, false,
                               true, /*IsInOut*/ false, /*isIUO*/ false,
                               /*isAutoClosure*/ false);
      builder.addComma();
      builder.addCallParameter(context.getIdentifier("alpha"), floatType, false,
                               true, /*IsInOut*/ false, /*isIUO*/ false,
                               /*isAutoClosure*/ false);
      builder.addRightParen();
    });

    auto stringType = context.getStringDecl()->getDeclaredType();
    addFromProto(LK::ImageLiteral, "", [&](Builder &builder) {
      builder.addTextChunk("#imageLiteral");
      builder.addLeftParen();
      builder.addCallParameter(context.getIdentifier("resourceName"),
                               stringType, false, true, /*IsInOut*/ false,
                               /*isIUO*/ false, /*isAutoClosure*/ false);
      builder.addRightParen();
    });

    // Add tuple completion (item, item).
    {
      CodeCompletionResultBuilder builder(Sink, CodeCompletionResult::Literal,
                                          SemanticContextKind::None, {});
      builder.setLiteralKind(LK::Tuple);

      builder.addLeftParen();
      builder.addSimpleNamedParameter("values");
      builder.addRightParen();
      for (auto T : ExpectedTypes) {
        if (!T)
          continue;
        if (T->is<TupleType>()) {
          addTypeAnnotation(builder, T);
          builder.setExpectedTypeRelation(CodeCompletionResult::Identical);
          break;
        }
      }
    }
  }

  struct FilteredDeclConsumer : public swift::VisibleDeclConsumer {
    swift::VisibleDeclConsumer &Consumer;
    DeclFilter Filter;
    FilteredDeclConsumer(swift::VisibleDeclConsumer &Consumer,
                         DeclFilter Filter) : Consumer(Consumer), Filter(Filter) {}
    void foundDecl(ValueDecl *VD, DeclVisibilityKind Kind) override {
      if (Filter(VD, Kind))
        Consumer.foundDecl(VD, Kind);
    }
  };

  void getValueCompletionsInDeclContext(SourceLoc Loc,
                                        DeclFilter Filter = DefaultFilter,
                                        bool IncludeTopLevel = false,
                                        bool RequestCache = true,
                                        bool LiteralCompletions = true) {
    ExprType = Type();
    Kind = LookupKind::ValueInDeclContext;
    NeedLeadingDot = false;
    FilteredDeclConsumer Consumer(*this, Filter);
    lookupVisibleDecls(Consumer, CurrDeclContext, TypeResolver,
                       /*IncludeTopLevel=*/IncludeTopLevel, Loc);
    if (RequestCache)
      RequestedCachedResults.push_back(RequestedResultsTy::toplevelResults());

    // Manually add any expected nominal types from imported modules so that
    // they get their expected type relation. Don't include protocols, since
    // they can't be initialized from the type name.
    // FIXME: this does not include types that conform to an expected protocol.
    // FIXME: this creates duplicate results.
    for (auto T : ExpectedTypes) {
      if (auto NT = T->getAs<NominalType>()) {
        if (auto NTD = NT->getDecl()) {
          if (!isa<ProtocolDecl>(NTD) &&
              NTD->getModuleContext() != CurrDeclContext->getParentModule()) {
            addNominalTypeRef(NT->getDecl(),
                              DeclVisibilityKind::VisibleAtTopLevel);
          }
        }
      }
    }

    if (CompletionContext) {
      // FIXME: this is an awful simplification that says all and only enums can
      // use implicit member syntax (leading dot). Computing the accurate answer
      // using lookup (e.g. getUnresolvedMemberCompletions) is too expensive,
      // and for some clients this approximation is good enough.
      CompletionContext->MayUseImplicitMemberExpr =
          std::any_of(ExpectedTypes.begin(), ExpectedTypes.end(), [](Type T) {
            if (auto *NTD = T->getAnyNominal())
              return isa<EnumDecl>(NTD);
            return false;
          });
    }

    if (LiteralCompletions)
      addValueLiteralCompletions();

    // If the expected type is ObjectiveC.Selector, add #selector. If
    // it's String, add #keyPath.
    if (Ctx.LangOpts.EnableObjCInterop) {
      bool addedSelector = false;
      bool addedKeyPath = false;
      for (auto T : ExpectedTypes) {
        T = T->lookThroughAllOptionalTypes();
        if (auto structDecl = T->getStructOrBoundGenericStruct()) {
          if (!addedSelector &&
              structDecl->getName() == Ctx.Id_Selector &&
              structDecl->getParentModule()->getName() == Ctx.Id_ObjectiveC) {
            addPoundSelector(/*needPound=*/true);
            if (addedKeyPath) break;
            addedSelector = true;
            continue;
          }
        }

        if (!addedKeyPath && T->getAnyNominal() == Ctx.getStringDecl()) {
          addPoundKeyPath(/*needPound=*/true);
          if (addedSelector) break;
          addedKeyPath = true;
          continue;
        }
      }
    }
  }

  void getUnresolvedMemberCompletions(Type T) {
    if (!T->mayHaveMembers())
      return;

    ModuleDecl *CurrModule = CurrDeclContext->getParentModule();

    // We can only say .foo where foo is a static member of the contextual
    // type and has the same type (or if the member is a function, then the
    // same result type) as the contextual type.
    FilteredDeclConsumer consumer(*this, [=](ValueDecl *VD,
                                             DeclVisibilityKind reason) {
      if (VD->isOperator())
        return false;

      if (!VD->hasInterfaceType()) {
        TypeResolver->resolveDeclSignature(VD);
        if (!VD->hasInterfaceType())
          return false;
      }

      // Enum element decls can always be referenced by implicit member
      // expression.
      if (isa<EnumElementDecl>(VD))
        return true;

      // Only non-failable constructors are implicitly referenceable.
      if (auto CD = dyn_cast<ConstructorDecl>(VD)) {
        switch (CD->getFailability()) {
          case OTK_None:
          case OTK_ImplicitlyUnwrappedOptional:
            return true;
          case OTK_Optional:
            return false;
        }
      }

      // Otherwise, check the result type matches the contextual type.
      auto declTy = T->getTypeOfMember(CurrModule, VD);
      if (declTy->is<ErrorType>())
        return false;

      DeclContext *DC = const_cast<DeclContext *>(CurrDeclContext);

      // Member types can also be implicitly referenceable as long as it's
      // convertible to the contextual type.
      if (auto CD = dyn_cast<TypeDecl>(VD)) {
        declTy = declTy->getMetatypeInstanceType();

        // Emit construction for the same type via typealias doesn't make sense
        // because we are emitting all `.init()`s.
        if (declTy->isEqual(T))
          return false;
        return swift::isConvertibleTo(declTy, T, *DC);
      }

      // Only static member can be referenced.
      if (!VD->isStatic())
        return false;

      if (isa<FuncDecl>(VD)) {
        // Strip '(Self.Type) ->' and parameters.
        declTy = declTy->castTo<AnyFunctionType>()->getResult();
        declTy = declTy->castTo<AnyFunctionType>()->getResult();
      } else if (auto FT = declTy->getAs<AnyFunctionType>()) {
        // The compiler accepts 'static var factory: () -> T' for implicit
        // member expression.
        // FIXME: This emits just 'factory'. We should emit 'factory()' instead.
        declTy = FT->getResult();
      }
      return declTy->isEqual(T) || swift::isConvertibleTo(declTy, T, *DC);
    });

    auto baseType = MetatypeType::get(T);
    llvm::SaveAndRestore<LookupKind> SaveLook(Kind, LookupKind::ValueExpr);
    llvm::SaveAndRestore<Type> SaveType(ExprType, baseType);
    llvm::SaveAndRestore<bool> SaveUnresolved(IsUnresolvedMember, true);
    lookupVisibleMemberDecls(consumer, baseType, CurrDeclContext,
                             TypeResolver,
                             /*includeInstanceMembers=*/false);
  }

  void getUnresolvedMemberCompletions(ArrayRef<Type> Types) {
    NeedLeadingDot = !HaveDot;
    for (auto T : Types) {
      if (!T)
        continue;
      if (auto objT = T->getOptionalObjectType()) {
        // If this is optional type, perform completion for the object type.
        // i.e. 'let _: Enum??? = .enumMember' is legal.
        getUnresolvedMemberCompletions(objT->lookThroughAllOptionalTypes());
      }
      getUnresolvedMemberCompletions(T);
    }
  }

  void addArgNameCompletionResults(ArrayRef<StringRef> Names) {
    for (auto Name : Names) {
      CodeCompletionResultBuilder Builder(Sink,
                                          CodeCompletionResult::ResultKind::Keyword,
                                          SemanticContextKind::ExpressionSpecific, {});
      Builder.addTextChunk(Name);
      Builder.addCallParameterColon();
      Builder.addTypeAnnotation("Argument name");
    }
  }

  void getTypeContextEnumElementCompletions(SourceLoc Loc) {
    llvm::SaveAndRestore<LookupKind> ChangeLookupKind(
        Kind, LookupKind::EnumElement);
    NeedLeadingDot = !HaveDot;

    auto *Switch = cast_or_null<SwitchStmt>(
        findNearestStmt(CurrDeclContext, Loc, StmtKind::Switch));
    if (!Switch)
      return;
    auto Ty = Switch->getSubjectExpr()->getType();
    if (!Ty)
      return;
    auto *TheEnumDecl = dyn_cast_or_null<EnumDecl>(Ty->getAnyNominal());
    if (!TheEnumDecl)
      return;
    for (auto Element : TheEnumDecl->getAllElements()) {
      foundDecl(Element, DeclVisibilityKind::MemberOfCurrentNominal);
    }
  }

  void getTypeCompletions(Type BaseType) {
    Kind = LookupKind::Type;
    this->BaseType = BaseType;
    NeedLeadingDot = !HaveDot;
    Type MetaBase = MetatypeType::get(BaseType);
    lookupVisibleMemberDecls(*this, MetaBase,
                             CurrDeclContext, TypeResolver,
                             IncludeInstanceMembers);
    addKeyword("Type", MetaBase);
  }

  static bool canUseAttributeOnDecl(DeclAttrKind DAK, bool IsInSil,
                                    Optional<DeclKind> DK) {
    if (DeclAttribute::isUserInaccessible(DAK))
      return false;
    if (DeclAttribute::isDeclModifier(DAK))
      return false;
    if (DeclAttribute::shouldBeRejectedByParser(DAK))
      return false;
    if (!IsInSil && DeclAttribute::isSilOnly(DAK))
      return false;
    if (!DK.hasValue())
      return true;
    return DeclAttribute::canAttributeAppearOnDeclKind(DAK, DK.getValue());
  }

  void getAttributeDeclCompletions(bool IsInSil, Optional<DeclKind> DK) {
    // FIXME: also include user-defined attribute keywords
    StringRef TargetName = "Declaration";
    if (DK.hasValue()) {
      switch (DK.getValue()) {
#define DECL(Id, ...)                                                         \
      case DeclKind::Id:                                                      \
        TargetName = #Id;                                                     \
        break;
#include "swift/AST/DeclNodes.def"
      }
    }
    std::string Description = TargetName.str() + " Attribute";
#define DECL_ATTR(KEYWORD, NAME, ...)                                         \
    if (canUseAttributeOnDecl(DAK_##NAME, IsInSil, DK))                       \
      addDeclAttrKeyword(#KEYWORD, Description);
#include "swift/AST/Attr.def"
  }

  void getAttributeDeclParamCompletions(DeclAttrKind AttrKind, int ParamIndex) {
    if (AttrKind == DAK_Available) {
      if (ParamIndex == 0) {
        addDeclAttrParamKeyword("*", "Platform", false);
#define AVAILABILITY_PLATFORM(X, PrettyName)                                  \
        addDeclAttrParamKeyword(#X, "Platform", false);
#include "swift/AST/PlatformKinds.def"
      } else {
        addDeclAttrParamKeyword("unavailable", "", false);
        addDeclAttrParamKeyword("message", "Specify message", true);
        addDeclAttrParamKeyword("renamed", "Specify replacing name", true);
        addDeclAttrParamKeyword("introduced", "Specify version number", true);
        addDeclAttrParamKeyword("deprecated", "Specify version number", true);
      }
    }
  }

  void collectPrecedenceGroups() {
    assert(CurrDeclContext);

    auto M = CurrDeclContext->getParentModule();

    if (M) {
      for (auto FU: M->getFiles()) {
        // We are looking through the current module,
        // inspect only source files.
        if (FU->getKind() != FileUnitKind::Source)
          continue;

        llvm::SmallVector<PrecedenceGroupDecl*, 4> results;
        cast<SourceFile>(FU)->getPrecedenceGroups(results);

        for (auto PG: results)
            addPrecedenceGroupRef(PG);
      }
    }
    CurrDeclContext->getParentSourceFile()
      ->forAllVisibleModules([&](ModuleDecl::ImportedModule Import) {
      auto Module = Import.second;
      if (Module == M)
        return;

      RequestedCachedResults.push_back(
        RequestedResultsTy::fromModule(Module).onlyPrecedenceGroups());
    });
  }

  void getPrecedenceGroupCompletions(SyntaxKind SK) {
    switch (SK) {
    case SyntaxKind::PrecedenceGroupAssociativity:
      addKeyword(getAssociativitySpelling(Associativity::None));
      addKeyword(getAssociativitySpelling(Associativity::Left));
      addKeyword(getAssociativitySpelling(Associativity::Right));
      break;
    case SyntaxKind::PrecedenceGroupAssignment:
      addKeyword(getTokenText(tok::kw_false), Type(), SemanticContextKind::None,
                 CodeCompletionKeywordKind::kw_false);
      addKeyword(getTokenText(tok::kw_true), Type(), SemanticContextKind::None,
                 CodeCompletionKeywordKind::kw_true);
      break;
    case SyntaxKind::PrecedenceGroupAttributeList:
      addKeyword("associativity");
      addKeyword("higherThan");
      addKeyword("lowerThan");
      addKeyword("assignment");
      break;
    case SyntaxKind::PrecedenceGroupRelation:
      collectPrecedenceGroups();
      break;
    default:
        llvm_unreachable("not a precedencegroup SyntaxKind");
    }
  }

  void getPoundAvailablePlatformCompletions() {

    // The platform names should be identical to those in @available.
    getAttributeDeclParamCompletions(DAK_Available, 0);
  }

  void getTypeCompletionsInDeclContext(SourceLoc Loc) {
    Kind = LookupKind::TypeInDeclContext;
    lookupVisibleDecls(*this, CurrDeclContext, TypeResolver,
                       /*IncludeTopLevel=*/false, Loc);

    RequestedCachedResults.push_back(
      RequestedResultsTy::toplevelResults().onlyTypes());
  }

  void getToplevelCompletions(bool OnlyTypes) {
    Kind = OnlyTypes ? LookupKind::TypeInDeclContext
                     : LookupKind::ValueInDeclContext;
    NeedLeadingDot = false;
    ModuleDecl *M = CurrDeclContext->getParentModule();
    AccessFilteringDeclConsumer FilteringConsumer(CurrDeclContext, *this);
    M->lookupVisibleDecls({}, FilteringConsumer, NLKind::UnqualifiedLookup);
  }

  void getVisibleDeclsOfModule(const ModuleDecl *TheModule,
                               ArrayRef<std::string> AccessPath,
                               bool ResultsHaveLeadingDot) {
    Kind = LookupKind::ImportFromModule;
    NeedLeadingDot = ResultsHaveLeadingDot;

    llvm::SmallVector<std::pair<Identifier, SourceLoc>, 1> LookupAccessPath;
    for (auto Piece : AccessPath) {
      LookupAccessPath.push_back(
          std::make_pair(Ctx.getIdentifier(Piece), SourceLoc()));
    }
    AccessFilteringDeclConsumer FilteringConsumer(CurrDeclContext, *this);
    TheModule->lookupVisibleDecls(LookupAccessPath, FilteringConsumer,
                                  NLKind::UnqualifiedLookup);

    llvm::SmallVector<PrecedenceGroupDecl*, 16> precedenceGroups;
    TheModule->getPrecedenceGroups(precedenceGroups);

    for (auto PGD: precedenceGroups)
      addPrecedenceGroupRef(PGD);
  }
};

class CompletionOverrideLookup : public swift::VisibleDeclConsumer {
  CodeCompletionResultSink &Sink;
  const DeclContext *CurrDeclContext;
  LazyResolver *TypeResolver;
  SmallVectorImpl<StringRef> &ParsedKeywords;

  bool hasFuncIntroducer = false;
  bool hasVarIntroducer = false;
  bool hasTypealiasIntroducer = false;
  bool hasInitializerModifier = false;
  bool hasAccessModifier = false;
  bool hasOverride = false;
  bool hasOverridabilityModifier = false;

public:
  CompletionOverrideLookup(CodeCompletionResultSink &Sink, ASTContext &Ctx,
                           const DeclContext *CurrDeclContext,
                           SmallVectorImpl<StringRef> &ParsedKeywords)
      : Sink(Sink),
        CurrDeclContext(CurrDeclContext), ParsedKeywords(ParsedKeywords) {
    (void)createTypeChecker(Ctx);
    TypeResolver = Ctx.getLazyResolver();

    hasFuncIntroducer = isKeywordSpecified("func");
    hasVarIntroducer = isKeywordSpecified("var") ||
                       isKeywordSpecified("let");
    hasTypealiasIntroducer = isKeywordSpecified("typealias");
    hasInitializerModifier = isKeywordSpecified("required") ||
                             isKeywordSpecified("convenience");
    hasAccessModifier = isKeywordSpecified("private") ||
                        isKeywordSpecified("fileprivate") ||
                        isKeywordSpecified("internal") ||
                        isKeywordSpecified("public") ||
                        isKeywordSpecified("open");
    hasOverride = isKeywordSpecified("override");
    hasOverridabilityModifier = isKeywordSpecified("final") ||
                                isKeywordSpecified("open");
  }

  bool isKeywordSpecified(StringRef Word) {
    return std::find(ParsedKeywords.begin(), ParsedKeywords.end(), Word)
      != ParsedKeywords.end();
  }

  bool missingOverride(DeclVisibilityKind Reason) {
    return !hasOverride && Reason == DeclVisibilityKind::MemberOfSuper &&
           !CurrDeclContext->getSelfProtocolDecl();
  }

  void addAccessControl(const ValueDecl *VD,
                        CodeCompletionResultBuilder &Builder) {
    assert(CurrDeclContext->getSelfNominalTypeDecl());
    auto AccessOfContext =
        CurrDeclContext->getSelfNominalTypeDecl()->getFormalAccess();
    auto Access = std::min(VD->getFormalAccess(), AccessOfContext);
    // Only emit 'public', not needed otherwise.
    if (Access >= AccessLevel::Public)
      Builder.addAccessControlKeyword(Access);
  }

  void addValueOverride(const ValueDecl *VD, DeclVisibilityKind Reason,
                        CodeCompletionResultBuilder &Builder,
                        bool hasDeclIntroducer) {

    class DeclNameOffsetLocatorPrinter : public StreamPrinter {
    public:
      using StreamPrinter::StreamPrinter;

      Optional<unsigned> NameOffset;

      void printDeclLoc(const Decl *D) override {
        if (!NameOffset.hasValue())
          NameOffset = OS.tell();
      }
    };

    llvm::SmallString<256> DeclStr;
    unsigned NameOffset = 0;
    {
      llvm::raw_svector_ostream OS(DeclStr);
      DeclNameOffsetLocatorPrinter Printer(OS);
      PrintOptions Options;
      if (auto transformType = CurrDeclContext->getDeclaredTypeInContext())
        Options.setBaseType(transformType);
      Options.PrintImplicitAttrs = false;
      Options.ExclusiveAttrList.push_back(TAK_escaping);
      Options.PrintOverrideKeyword = false;
      Options.PrintPropertyAccessors = false;
      VD->print(Printer, Options);
      NameOffset = Printer.NameOffset.getValue();
    }

    if (!hasDeclIntroducer && !hasAccessModifier)
      addAccessControl(VD, Builder);

    // FIXME: if we're missing 'override', but have the decl introducer we
    // should delete it and re-add both in the correct order.
    if (!hasDeclIntroducer && missingOverride(Reason))
      Builder.addOverrideKeyword();

    if (!hasDeclIntroducer)
      Builder.addDeclIntroducer(DeclStr.str().substr(0, NameOffset));

    Builder.addTextChunk(DeclStr.str().substr(NameOffset));
  }

  void addMethodOverride(const FuncDecl *FD, DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        SemanticContextKind::Super, {});
    Builder.setAssociatedDecl(FD);
    addValueOverride(FD, Reason, Builder, hasFuncIntroducer);
    Builder.addBraceStmtWithCursor();
  }

  void addVarOverride(const VarDecl *VD, DeclVisibilityKind Reason) {
    // Overrides cannot use 'let', but if the 'override' keyword is specified
    // then the intention is clear, so provide the results anyway.  The compiler
    // can then provide an error telling you to use 'var' instead.
    // If we don't need override then it's a protocol requirement, so show it.
    if (missingOverride(Reason) && hasVarIntroducer &&
        isKeywordSpecified("let"))
      return;

    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        SemanticContextKind::Super, {});
    Builder.setAssociatedDecl(VD);
    addValueOverride(VD, Reason, Builder, hasVarIntroducer);
  }

  void addTypeAlias(const AssociatedTypeDecl *ATD, DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(Sink,
      CodeCompletionResult::ResultKind::Declaration,
      SemanticContextKind::Super, {});
    Builder.setAssociatedDecl(ATD);
    if (!hasTypealiasIntroducer && !hasAccessModifier)
      addAccessControl(ATD, Builder);
    if (!hasTypealiasIntroducer)
      Builder.addDeclIntroducer("typealias ");
    Builder.addTextChunk(ATD->getName().str());
    Builder.addTextChunk(" = ");
    Builder.addSimpleNamedParameter("Type");
  }

  void addConstructor(const ConstructorDecl *CD, DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        SemanticContextKind::Super, {});
    Builder.setAssociatedDecl(CD);

    if (!hasAccessModifier)
      addAccessControl(CD, Builder);

    if (missingOverride(Reason) && CD->isDesignatedInit() && !CD->isRequired())
      Builder.addOverrideKeyword();

    // Emit 'required' if we're in class context, 'required' is not specified,
    // and 1) this is a protocol conformance and the class is not final, or 2)
    // this is subclass and the initializer is marked as required.
    bool needRequired = false;
    auto C = CurrDeclContext->getSelfClassDecl();
    if (C && !isKeywordSpecified("required")) {
      if (Reason ==
            DeclVisibilityKind::MemberOfProtocolImplementedByCurrentNominal &&
          !C->isFinal())
        needRequired = true;
      else if (Reason == DeclVisibilityKind::MemberOfSuper && CD->isRequired())
        needRequired = true;
    }

    llvm::SmallString<256> DeclStr;
    if (needRequired)
      DeclStr += "required ";
    {
      llvm::raw_svector_ostream OS(DeclStr);
      PrintOptions Options;
      Options.PrintImplicitAttrs = false;
      Options.SkipAttributes = true;
      CD->print(OS, Options);
    }
    Builder.addTextChunk(DeclStr);
    Builder.addBraceStmtWithCursor();
  }

  // Implement swift::VisibleDeclConsumer.
  void foundDecl(ValueDecl *D, DeclVisibilityKind Reason) override {
    if (Reason == DeclVisibilityKind::MemberOfCurrentNominal)
      return;

    if (D->shouldHideFromEditor())
      return;

    if (D->getAttrs().hasAttribute<FinalAttr>())
      return;

    if (!D->hasInterfaceType())
      TypeResolver->resolveDeclSignature(D);

    bool hasIntroducer = hasFuncIntroducer ||
                         hasVarIntroducer ||
                         hasTypealiasIntroducer;

    if (auto *FD = dyn_cast<FuncDecl>(D)) {
      // We cannot override operators as members.
      if (FD->isBinaryOperator() || FD->isUnaryOperator())
        return;

      // We cannot override individual accessors.
      if (isa<AccessorDecl>(FD))
        return;

      if (hasFuncIntroducer || (!hasIntroducer && !hasInitializerModifier))
        addMethodOverride(FD, Reason);
      return;
    }

    if (auto *VD = dyn_cast<VarDecl>(D)) {
      if (hasVarIntroducer || (!hasIntroducer && !hasInitializerModifier))
        addVarOverride(VD, Reason);
      return;
    }

    if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
      if (!isa<ProtocolDecl>(CD->getDeclContext()))
        return;
      if (hasIntroducer || hasOverride || hasOverridabilityModifier)
        return;
      if (CD->isRequired() || CD->isDesignatedInit())
        addConstructor(CD, Reason);
      return;
    }
  }

  void addDesignatedInitializers(NominalTypeDecl *NTD) {
    if (hasFuncIntroducer || hasVarIntroducer || hasTypealiasIntroducer ||
        hasOverridabilityModifier)
      return;

    const auto *CD = dyn_cast<ClassDecl>(NTD);
    if (!CD)
      return;
    if (!CD->hasSuperclass())
      return;
    CD = CD->getSuperclassDecl();
    for (const auto *Member : CD->getMembers()) {
      const auto *Constructor = dyn_cast<ConstructorDecl>(Member);
      if (!Constructor)
        continue;
      if (Constructor->hasStubImplementation())
        continue;
      if (Constructor->isDesignatedInit())
        addConstructor(Constructor, DeclVisibilityKind::MemberOfSuper);
    }
  }

  void addAssociatedTypes(NominalTypeDecl *NTD) {
    if (!hasTypealiasIntroducer &&
        (hasFuncIntroducer || hasVarIntroducer || hasInitializerModifier ||
         hasOverride || hasOverridabilityModifier))
      return;

    for (auto Conformance : NTD->getAllConformances()) {
      auto Proto = Conformance->getProtocol();
      if (!Proto->isAccessibleFrom(CurrDeclContext))
        continue;
      auto NormalConformance = Conformance->getRootNormalConformance();
      for (auto Member : Proto->getMembers()) {
        auto *ATD = dyn_cast<AssociatedTypeDecl>(Member);
        if (!ATD)
          continue;
        // FIXME: Also exclude the type alias that has already been specified.
        if (!NormalConformance->hasTypeWitness(ATD) ||
            !ATD->getDefaultDefinitionLoc().isNull())
          continue;
        addTypeAlias(ATD,
          DeclVisibilityKind::MemberOfProtocolImplementedByCurrentNominal);
      }
    }
  }

  void getOverrideCompletions(SourceLoc Loc) {
    if (!CurrDeclContext->isTypeContext())
      return;
    if (isa<ProtocolDecl>(CurrDeclContext))
      return;

    Type CurrTy = CurrDeclContext->getSelfTypeInContext();
    auto *NTD = CurrDeclContext->getSelfNominalTypeDecl();
    if (CurrTy && !CurrTy->is<ErrorType>()) {
      lookupVisibleMemberDecls(*this, CurrTy, CurrDeclContext,
                               TypeResolver,
                               /*includeInstanceMembers=*/false);
      addDesignatedInitializers(NTD);
      addAssociatedTypes(NTD);
    }
  }
};
} // end anonymous namespace

static void addSelectorModifierKeywords(CodeCompletionResultSink &sink) {
  auto addKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind) {
    CodeCompletionResultBuilder Builder(
                                  sink,
                                  CodeCompletionResult::ResultKind::Keyword,
                                  SemanticContextKind::None, {});
    Builder.setKeywordKind(Kind);
    Builder.addTextChunk(Name);
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("@objc property", /*IsVarArg=*/false);
  };

  addKeyword("getter", CodeCompletionKeywordKind::None);
  addKeyword("setter", CodeCompletionKeywordKind::None);
}

void CodeCompletionCallbacksImpl::completeDotExpr(Expr *E, SourceLoc DotLoc) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::DotExpr;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
  }

  ParsedExpr = E;
  this->DotLoc = DotLoc;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeStmtOrExpr() {
  assert(P.Tok.is(tok::code_complete));
  Kind = CompletionKind::StmtOrExpr;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePostfixExprBeginning(CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExprBeginning;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
    if (CompleteExprSelectorContext == ObjCSelectorContext::MethodSelector) {
      addSelectorModifierKeywords(CompletionContext.getResultSink());
    }
  }


  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeForEachSequenceBeginning(
    CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));
  Kind = CompletionKind::ForEachSequence;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completePostfixExpr(Expr *E, bool hasSpace) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  HasSpace = hasSpace;
  Kind = CompletionKind::PostfixExpr;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
  }

  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePostfixExprParen(Expr *E,
                                                           Expr *CodeCompletionE) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExprParen;
  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = static_cast<CodeCompletionExpr*>(CodeCompletionE);

  ShouldCompleteCallPatternAfterParen = true;
  if (Context.LangOpts.CodeCompleteCallPatternHeuristics) {
    // Lookahead one token to decide what kind of call completions to provide.
    // When it appears that there is already code for the call present, just
    // complete values and/or argument labels.  Otherwise give the entire call
    // pattern.
    Token next = P.peekToken();
    if (!next.isAtStartOfLine() && !next.is(tok::eof) && !next.is(tok::r_paren)) {
      ShouldCompleteCallPatternAfterParen = false;
    }
  }
}

void CodeCompletionCallbacksImpl::completeExprSuper(SuperRefExpr *SRE) {
  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::SuperExpr;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
  }

  ParsedExpr = SRE;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeExprSuperDot(SuperRefExpr *SRE) {
  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::SuperExprDot;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
  }

  ParsedExpr = SRE;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeExprKeyPath(KeyPathExpr *KPE,
                                                      SourceLoc DotLoc) {
  Kind = (!KPE || KPE->isObjC()) ? CompletionKind::KeyPathExprObjC
                                 : CompletionKind::KeyPathExprSwift;
  ParsedExpr = KPE;
  this->DotLoc = DotLoc;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePoundAvailablePlatform() {
  Kind = CompletionKind::PoundAvailablePlatform;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeSimpleBeginning() {
  Kind = CompletionKind::TypeSimpleBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeDeclAttrParam(DeclAttrKind DK,
                                                        int Index) {
  Kind = CompletionKind::AttributeDeclParen;
  AttrKind = DK;
  AttrParamIndex = Index;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeDeclAttrKeyword(Decl *D,
                                                          bool Sil,
                                                          bool Param) {
  Kind = CompletionKind::AttributeBegin;
  IsInSil = Sil;
  if (Param) {
    AttTargetDK = DeclKind::Param;
  } else if (D) {
    AttTargetDK = D->getKind();
  }
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeInPrecedenceGroup(SyntaxKind SK) {
  assert(P.Tok.is(tok::code_complete));

  SyntxKind = SK;
  Kind = CompletionKind::PrecedenceGroup;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeIdentifierWithDot(
    IdentTypeRepr *ITR) {
  if (!ITR) {
    completeTypeSimpleBeginning();
    return;
  }
  Kind = CompletionKind::TypeIdentifierWithDot;
  ParsedTypeLoc = TypeLoc(ITR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeIdentifierWithoutDot(
    IdentTypeRepr *ITR) {
  assert(ITR);
  Kind = CompletionKind::TypeIdentifierWithoutDot;
  ParsedTypeLoc = TypeLoc(ITR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeCaseStmtBeginning() {
  assert(!InEnumElementRawValue);

  Kind = CompletionKind::CaseStmtBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeCaseStmtDotPrefix() {
  assert(!InEnumElementRawValue);

  Kind = CompletionKind::CaseStmtDotPrefix;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeImportDecl(
    std::vector<std::pair<Identifier, SourceLoc>> &Path) {
  Kind = CompletionKind::Import;
  CurDeclContext = P.CurDeclContext;
  DotLoc = Path.empty() ? SourceLoc() : Path.back().second;
  if (DotLoc.isInvalid())
    return;
  auto Importer = static_cast<ClangImporter *>(CurDeclContext->getASTContext().
                                               getClangModuleLoader());
  std::vector<std::string> SubNames;
  Importer->collectSubModuleNames(Path, SubNames);
  ASTContext &Ctx = CurDeclContext->getASTContext();
  for (StringRef Sub : SubNames) {
    Path.push_back(std::make_pair(Ctx.getIdentifier(Sub), SourceLoc()));
    SubModuleNameVisibilityPairs.push_back(
      std::make_pair(Sub.str(), Ctx.getLoadedModule(Path)));
    Path.pop_back();
  }
}

void CodeCompletionCallbacksImpl::completeUnresolvedMember(CodeCompletionExpr *E,
    SourceLoc DotLoc) {
  Kind = CompletionKind::UnresolvedMember;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  this->DotLoc = DotLoc;
}

void CodeCompletionCallbacksImpl::completeAssignmentRHS(AssignExpr *E) {
  AssignmentExpr = E;
  ParsedExpr = E->getDest();
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::AssignmentRHS;
}

void CodeCompletionCallbacksImpl::completeCallArg(CodeCompletionExpr *E) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::CallArg;
}

void CodeCompletionCallbacksImpl::completeReturnStmt(CodeCompletionExpr *E) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::ReturnStmtExpr;
}

void CodeCompletionCallbacksImpl::completeYieldStmt(CodeCompletionExpr *E,
                                                    Optional<unsigned> index) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  // TODO: use a different completion kind when completing without an index
  // in a multiple-value context.
  Kind = CompletionKind::YieldStmtExpr;
}

void CodeCompletionCallbacksImpl::completeAfterPoundExpr(
    CodeCompletionExpr *E, Optional<StmtKind> ParentKind) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::AfterPoundExpr;
  ParentStmtKind = ParentKind;
}

void CodeCompletionCallbacksImpl::completeAfterPoundDirective() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::AfterPoundDirective;
}

void CodeCompletionCallbacksImpl::completePlatformCondition() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::PlatformConditon;
}

void CodeCompletionCallbacksImpl::completeAfterIfStmt(bool hasElse) {
  CurDeclContext = P.CurDeclContext;
  if (hasElse) {
    Kind = CompletionKind::AfterIfStmtElse;
  } else {
    Kind = CompletionKind::StmtOrExpr;
  }
}

void CodeCompletionCallbacksImpl::completeGenericParams(TypeLoc TL) {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::GenericParams;
  ParsedTypeLoc = TL;
}

void CodeCompletionCallbacksImpl::completeNominalMemberBeginning(
    SmallVectorImpl<StringRef> &Keywords) {
  assert(!InEnumElementRawValue);
  ParsedKeywords.clear();
  ParsedKeywords.append(Keywords.begin(), Keywords.end());
  Kind = CompletionKind::NominalMemberBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeAccessorBeginning() {
  Kind = CompletionKind::AccessorBeginning;
  CurDeclContext = P.CurDeclContext;
}

static bool isDynamicLookup(Type T) {
  return T->getRValueType()->isAnyObject();
}

static bool isClangSubModule(ModuleDecl *TheModule) {
  if (auto ClangMod = TheModule->findUnderlyingClangModule())
    return ClangMod->isSubModule();
  return false;
}

static void addKeyword(CodeCompletionResultSink &Sink, StringRef Name,
                       CodeCompletionKeywordKind Kind,
                       StringRef TypeAnnotation = "") {
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, {});
    Builder.setKeywordKind(Kind);
    Builder.addTextChunk(Name);
    if (!TypeAnnotation.empty())
      Builder.addTypeAnnotation(TypeAnnotation);
}

static void addDeclKeywords(CodeCompletionResultSink &Sink) {
  auto AddDeclKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind,
                        Optional<DeclAttrKind> DAK) {
    if (Name == "let" || Name == "var") {
      // Treat keywords that could be the start of a pattern specially.
      return;
    }

    // Remove user inaccessible keywords.
    if (DAK.hasValue() && DeclAttribute::isUserInaccessible(*DAK)) return;

    addKeyword(Sink, Name, Kind);
  };

#define DECL_KEYWORD(kw) AddDeclKeyword(#kw, CodeCompletionKeywordKind::kw_##kw, None);
#include "swift/Syntax/TokenKinds.def"

  // Context-sensitive keywords.
  auto AddCSKeyword = [&](StringRef Name, DeclAttrKind Kind) {
    AddDeclKeyword(Name, CodeCompletionKeywordKind::None, Kind);
  };

#define CONTEXTUAL_CASE(KW, CLASS) AddCSKeyword(#KW, DAK_##CLASS);
#define CONTEXTUAL_DECL_ATTR(KW, CLASS, ...) CONTEXTUAL_CASE(KW, CLASS)
#define CONTEXTUAL_DECL_ATTR_ALIAS(KW, CLASS) CONTEXTUAL_CASE(KW, CLASS)
#define CONTEXTUAL_SIMPLE_DECL_ATTR(KW, CLASS, ...) CONTEXTUAL_CASE(KW, CLASS)
#include <swift/AST/Attr.def>
#undef CONTEXTUAL_CASE

}

static void addStmtKeywords(CodeCompletionResultSink &Sink, bool MaybeFuncBody) {
  auto AddStmtKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind) {
    if (!MaybeFuncBody && Kind == CodeCompletionKeywordKind::kw_return)
      return;
    addKeyword(Sink, Name, Kind);
  };
#define STMT_KEYWORD(kw) AddStmtKeyword(#kw, CodeCompletionKeywordKind::kw_##kw);
#include "swift/Syntax/TokenKinds.def"
}

static void addLetVarKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "let", CodeCompletionKeywordKind::kw_let);
  addKeyword(Sink, "var", CodeCompletionKeywordKind::kw_var);
}

static void addAccessorKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "get", CodeCompletionKeywordKind::None);
  addKeyword(Sink, "set", CodeCompletionKeywordKind::None);
}

static void addObserverKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "willSet", CodeCompletionKeywordKind::None);
  addKeyword(Sink, "didSet", CodeCompletionKeywordKind::None);
}

static void addExprKeywords(CodeCompletionResultSink &Sink) {
  // Expr keywords.
  addKeyword(Sink, "try", CodeCompletionKeywordKind::kw_try);
  addKeyword(Sink, "try!", CodeCompletionKeywordKind::kw_try);
  addKeyword(Sink, "try?", CodeCompletionKeywordKind::kw_try);
  // FIXME: The pedantically correct way to find the type is to resolve the
  // Swift.StringLiteralType type.
  addKeyword(Sink, "#function", CodeCompletionKeywordKind::pound_function, "String");
  addKeyword(Sink, "#file", CodeCompletionKeywordKind::pound_file, "String");
  // Same: Swift.IntegerLiteralType.
  addKeyword(Sink, "#line", CodeCompletionKeywordKind::pound_line, "Int");
  addKeyword(Sink, "#column", CodeCompletionKeywordKind::pound_column, "Int");
  addKeyword(Sink, "#dsohandle", CodeCompletionKeywordKind::pound_dsohandle, "UnsafeMutableRawPointer");
}

static void addAnyTypeKeyword(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "Any", CodeCompletionKeywordKind::None, "Any");
}


void CodeCompletionCallbacksImpl::addKeywords(CodeCompletionResultSink &Sink,
                                              bool MaybeFuncBody) {
  switch (Kind) {
  case CompletionKind::None:
  case CompletionKind::DotExpr:
  case CompletionKind::AttributeDeclParen:
  case CompletionKind::AttributeBegin:
  case CompletionKind::PoundAvailablePlatform:
  case CompletionKind::Import:
  case CompletionKind::UnresolvedMember:
  case CompletionKind::CallArg:
  case CompletionKind::AfterPoundExpr:
  case CompletionKind::AfterPoundDirective:
  case CompletionKind::PlatformConditon:
  case CompletionKind::GenericParams:
  case CompletionKind::KeyPathExprObjC:
  case CompletionKind::KeyPathExprSwift:
  case CompletionKind::PrecedenceGroup:
    break;

  case CompletionKind::AccessorBeginning: {
    // TODO: Omit already declared or mutally exclusive accessors.
    //       E.g. If 'get' is already declared, emit 'set' only.
    addAccessorKeywords(Sink);

    // Only 'var' for non-protocol context can have 'willSet' and 'didSet'.
    assert(ParsedDecl);
    VarDecl *var = dyn_cast<VarDecl>(ParsedDecl);
    if (auto accessor = dyn_cast<AccessorDecl>(ParsedDecl))
      var = dyn_cast<VarDecl>(accessor->getStorage());
    if (var && !var->getDeclContext()->getSelfProtocolDecl())
      addObserverKeywords(Sink);

    if (!isa<AccessorDecl>(ParsedDecl))
      break;

    MaybeFuncBody = true;
    LLVM_FALLTHROUGH;
  }
  case CompletionKind::StmtOrExpr:
    addDeclKeywords(Sink);
    addStmtKeywords(Sink, MaybeFuncBody);
    LLVM_FALLTHROUGH;
  case CompletionKind::AssignmentRHS:
  case CompletionKind::ReturnStmtExpr:
  case CompletionKind::YieldStmtExpr:
  case CompletionKind::PostfixExprBeginning:
  case CompletionKind::ForEachSequence:
    addSuperKeyword(Sink);
    addLetVarKeywords(Sink);
    addExprKeywords(Sink);
    addAnyTypeKeyword(Sink);
    break;

  case CompletionKind::PostfixExpr:
  case CompletionKind::PostfixExprParen:
  case CompletionKind::SuperExpr:
  case CompletionKind::SuperExprDot:
  case CompletionKind::CaseStmtBeginning:
  case CompletionKind::CaseStmtDotPrefix:
  case CompletionKind::TypeIdentifierWithDot:
  case CompletionKind::TypeIdentifierWithoutDot:
    break;

  case CompletionKind::TypeSimpleBeginning:
    addAnyTypeKeyword(Sink);
    break;
      
  case CompletionKind::NominalMemberBeginning: {
    bool HasDeclIntroducer = llvm::find_if(ParsedKeywords, [](const StringRef kw) {
      return llvm::StringSwitch<bool>(kw)
        .Case("associatedtype", true)
        .Case("class", true)
        .Case("deinit", true)
        .Case("enum", true)
        .Case("extension", true)
        .Case("func", true)
        .Case("import", true)
        .Case("init", true)
        .Case("let", true)
        .Case("operator", true)
        .Case("precedencegroup", true)
        .Case("protocol", true)
        .Case("struct", true)
        .Case("subscript", true)
        .Case("typealias", true)
        .Case("var", true)
        .Default(false);
    }) != ParsedKeywords.end();
    if (!HasDeclIntroducer) {
      addDeclKeywords(Sink);
      addLetVarKeywords(Sink);
    }
    break;
  }

  case CompletionKind::AfterIfStmtElse:
    addKeyword(Sink, "if", CodeCompletionKeywordKind::kw_if);
    break;
  }
}

static void addPoundDirectives(CodeCompletionResultSink &Sink) {
  auto addWithName =
      [&](StringRef name, CodeCompletionKeywordKind K,
          llvm::function_ref<void(CodeCompletionResultBuilder &)> consumer =
              nullptr) {
        CodeCompletionResultBuilder Builder(Sink, CodeCompletionResult::Keyword,
                                            SemanticContextKind::None, {});
        Builder.addTextChunk(name);
        Builder.setKeywordKind(K);
        if (consumer)
          consumer(Builder);
      };

  addWithName("sourceLocation", CodeCompletionKeywordKind::pound_sourceLocation,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("file");
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("String");
    Builder.addComma();
    Builder.addTextChunk("line");
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("Int");
    Builder.addRightParen();
  });
  addWithName("warning", CodeCompletionKeywordKind::pound_warning,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("\"");
    Builder.addSimpleNamedParameter("message");
    Builder.addTextChunk("\"");
    Builder.addRightParen();
  });
  addWithName("error", CodeCompletionKeywordKind::pound_error,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("\"");
    Builder.addSimpleNamedParameter("message");
    Builder.addTextChunk("\"");
    Builder.addRightParen();
  });

  addWithName("if ", CodeCompletionKeywordKind::pound_if,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("condition");
  });

  // FIXME: These directives are only valid in conditional completion block.
  addWithName("elseif ", CodeCompletionKeywordKind::pound_elseif,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("condition");
  });
  addWithName("else", CodeCompletionKeywordKind::pound_else);
  addWithName("endif", CodeCompletionKeywordKind::pound_endif);
}

/// Add platform conditions used in '#if' and '#elseif' directives.
static void addPlatformConditions(CodeCompletionResultSink &Sink) {
  auto addWithName =
      [&](StringRef Name,
          llvm::function_ref<void(CodeCompletionResultBuilder & Builder)>
              consumer) {
        CodeCompletionResultBuilder Builder(
            Sink, CodeCompletionResult::ResultKind::Pattern,
            SemanticContextKind::ExpressionSpecific, {});
        Builder.addTextChunk(Name);
        Builder.addLeftParen();
        consumer(Builder);
        Builder.addRightParen();
      };
  addWithName("os", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("name");
  });
  addWithName("arch", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("name");
  });
  addWithName("canImport", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("module");
  });
  addWithName("targetEnvironment", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("simulator");
  });
  addWithName("swift", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk(">=");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("swift", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("<");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("compiler", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk(">=");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("compiler", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("<");
    Builder.addSimpleNamedParameter("version");
  });

  addKeyword(Sink, "true", CodeCompletionKeywordKind::kw_true, "Bool");
  addKeyword(Sink, "false", CodeCompletionKeywordKind::kw_false, "Bool");
}

/// Add flags specified by '-D' to completion results.
static void addConditionalCompilationFlags(ASTContext &Ctx,
                                           CodeCompletionResultSink &Sink) {
  for (auto Flag : Ctx.LangOpts.getCustomConditionalCompilationFlags()) {
    // TODO: Should we filter out some flags?
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::ExpressionSpecific, {});
    Builder.addTextChunk(Flag);
  }
}

void CodeCompletionCallbacksImpl::doneParsing() {
  CompletionContext.CodeCompletionKind = Kind;

  if (Kind == CompletionKind::None) {
    return;
  }

  bool MaybeFuncBody = true;
  if (CurDeclContext) {
    auto *CD = CurDeclContext->getLocalContext();
    if (!CD || CD->getContextKind() == DeclContextKind::Initializer ||
        CD->getContextKind() == DeclContextKind::TopLevelCodeDecl)
      MaybeFuncBody = false;
  }
  // Add keywords even if type checking fails completely.
  addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);

  if (auto *DC = dyn_cast_or_null<DeclContext>(ParsedDecl)) {
    if (DC->isChildContextOf(CurDeclContext))
      CurDeclContext = DC;
  }

  typeCheckContextUntil(
      CurDeclContext,
      CurDeclContext->getASTContext().SourceMgr.getCodeCompletionLoc());

  Optional<Type> ExprType;
  ConcreteDeclRef ReferencedDecl = nullptr;
  if (ParsedExpr) {
    if (auto typechecked = typeCheckParsedExpr()) {
      ExprType = typechecked->first;
      ReferencedDecl = typechecked->second;
      ParsedExpr->setType(*ExprType);
    }

    if (!ExprType && Kind != CompletionKind::PostfixExprParen &&
        Kind != CompletionKind::CallArg &&
        Kind != CompletionKind::KeyPathExprObjC)
      return;
  }

  if (!ParsedTypeLoc.isNull() && !typecheckParsedType())
    return;

  CompletionLookup Lookup(CompletionContext.getResultSink(), P.Context,
                          CurDeclContext, &CompletionContext);
  if (ExprType) {
    Lookup.setIsStaticMetatype(ParsedExpr->isStaticallyDerivedMetatype());
  }
  if (auto *DRE = dyn_cast_or_null<DeclRefExpr>(ParsedExpr)) {
    Lookup.setIsSelfRefExpr(DRE->getDecl()->getFullName() == Context.Id_self);
  }

  if (isInsideObjCSelector())
    Lookup.includeInstanceMembers();
  if (PreferFunctionReferencesToCalls)
    Lookup.setPreferFunctionReferencesToCalls();

  auto DoPostfixExprBeginning = [&] (){
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getValueCompletionsInDeclContext(Loc);
  };

  switch (Kind) {
  case CompletionKind::None:
    llvm_unreachable("should be already handled");
    return;

  case CompletionKind::DotExpr: {
    Lookup.setHaveDot(DotLoc);

    if (isDynamicLookup(*ExprType))
      Lookup.setIsDynamicLookup();

    if (!ExprType.getValue()->getAs<ModuleType>())
      Lookup.addKeyword(getTokenText(tok::kw_self),
                        (*ExprType)->getRValueType(),
                        SemanticContextKind::CurrentNominal,
                        CodeCompletionKeywordKind::kw_self);

    if (isa<BindOptionalExpr>(ParsedExpr) || isa<ForceValueExpr>(ParsedExpr))
      Lookup.setIsUnwrappedOptional(true);

    ExprContextInfo ContextInfo(CurDeclContext, ParsedExpr);
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes());
    Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    break;
  }

  case CompletionKind::KeyPathExprSwift: {
    auto KPE = dyn_cast<KeyPathExpr>(ParsedExpr);
    auto BGT = (*ExprType)->getAs<BoundGenericType>();
    if (!KPE || !BGT || BGT->getGenericArgs().size() != 2)
      break;
    assert(!KPE->isObjC());

    if (DotLoc.isValid())
      Lookup.setHaveDot(DotLoc);

    bool OnRoot = !KPE->getComponents().front().isValid();
    Lookup.setIsSwiftKeyPathExpr(OnRoot);

    auto ParsedType = BGT->getGenericArgs()[1];
    auto Components = KPE->getComponents();
    if (Components.back().getKind() ==
        KeyPathExpr::Component::Kind::OptionalWrap) {
      // KeyPath expr with '?' (e.g. '\Ty.[0].prop?.another').
      // Althogh expected type is optional, we should unwrap it because it's
      // unwrapped.
      ParsedType = ParsedType->getOptionalObjectType();
    }

    // The second generic type argument of KeyPath<Root, Value> should be
    // the value we pull code completion results from.
    Lookup.getValueExprCompletions(ParsedType);
    break;
  }

  case CompletionKind::StmtOrExpr:
    DoPostfixExprBeginning();
    break;

  case CompletionKind::ForEachSequence:
  case CompletionKind::PostfixExprBeginning: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes());
    DoPostfixExprBeginning();
    break;
  }

  case CompletionKind::PostfixExpr: {
    Lookup.setHaveLeadingSpace(HasSpace);
    if (isDynamicLookup(*ExprType))
      Lookup.setIsDynamicLookup();
    Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    Lookup.getOperatorCompletions(ParsedExpr, leadingSequenceExprs);

    if (!ExprType.getValue()->getAs<ModuleType>())
      Lookup.addKeyword(getTokenText(tok::kw_self),
                        (*ExprType)->getRValueType(),
                        SemanticContextKind::CurrentNominal,
                        CodeCompletionKeywordKind::kw_self);
    break;
  }

  case CompletionKind::PostfixExprParen: {
    Lookup.setHaveLParen(true);

    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes());
    if (ShouldCompleteCallPatternAfterParen) {
      if (ExprType) {
        Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
      } else {
        for (auto &typeAndDecl : ContextInfo.getPossibleCallees())
          Lookup.getValueExprCompletions(typeAndDecl.first, typeAndDecl.second);
      }
    } else {
      // Add argument labels, then fallthrough to get values.
      Lookup.addArgNameCompletionResults(ContextInfo.getPossibleNames());
    }

    if (!Lookup.FoundFunctionCalls ||
        (Lookup.FoundFunctionCalls &&
         Lookup.FoundFunctionsWithoutFirstKeyword)) {
      Lookup.setHaveLParen(false);
      DoPostfixExprBeginning();
    }
    break;
  }

  case CompletionKind::SuperExpr: {
    Lookup.setIsSuperRefExpr();
    Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    break;
  }

  case CompletionKind::SuperExprDot: {
    Lookup.setIsSuperRefExpr();
    Lookup.setHaveDot(SourceLoc());
    Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    break;
  }

  case CompletionKind::KeyPathExprObjC: {
    if (DotLoc.isValid())
      Lookup.setHaveDot(DotLoc);
    Lookup.setIsKeyPathExpr();
    Lookup.includeInstanceMembers();

    if (ExprType) {
      if (isDynamicLookup(*ExprType))
        Lookup.setIsDynamicLookup();

      Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    } else {
      SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
      Lookup.getValueCompletionsInDeclContext(Loc, KeyPathFilter,
                                              false, true, false);
    }
    break;
  }

  case CompletionKind::TypeSimpleBeginning: {
    Lookup.getTypeCompletionsInDeclContext(
        P.Context.SourceMgr.getCodeCompletionLoc());
    break;
  }

  case CompletionKind::TypeIdentifierWithDot: {
    Lookup.setHaveDot(SourceLoc());
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::TypeIdentifierWithoutDot: {
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::CaseStmtBeginning: {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getValueCompletionsInDeclContext(Loc);
    Lookup.getTypeContextEnumElementCompletions(Loc);
    break;
  }

  case CompletionKind::CaseStmtDotPrefix: {
    Lookup.setHaveDot(SourceLoc());
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getTypeContextEnumElementCompletions(Loc);
    break;
  }

  case CompletionKind::NominalMemberBeginning: {
    CompletionOverrideLookup OverrideLookup(CompletionContext.getResultSink(),
                                            P.Context, CurDeclContext,
                                            ParsedKeywords);
    OverrideLookup.getOverrideCompletions(SourceLoc());
    break;
  }

  case CompletionKind::AccessorBeginning: {
    if (isa<AccessorDecl>(ParsedDecl))
      DoPostfixExprBeginning();
    break;
  }

  case CompletionKind::AttributeBegin: {
    Lookup.getAttributeDeclCompletions(IsInSil, AttTargetDK);
    break;
  }
  case CompletionKind::AttributeDeclParen: {
    Lookup.getAttributeDeclParamCompletions(AttrKind, AttrParamIndex);
    break;
  }
  case CompletionKind::PoundAvailablePlatform: {
    Lookup.getPoundAvailablePlatformCompletions();
    break;
  }
  case CompletionKind::Import: {
    if (DotLoc.isValid())
      Lookup.addSubModuleNames(SubModuleNameVisibilityPairs);
    else
      Lookup.addImportModuleNames();
    break;
  }
  case CompletionKind::UnresolvedMember: {
    Lookup.setHaveDot(DotLoc);
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes());
    Lookup.getUnresolvedMemberCompletions(ContextInfo.getPossibleTypes());
    break;
  }
  case CompletionKind::AssignmentRHS : {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    if (auto destType = ParsedExpr->getType())
      Lookup.setExpectedTypes(destType->getRValueType());
    Lookup.getValueCompletionsInDeclContext(Loc, DefaultFilter);
    break;
  }
  case CompletionKind::CallArg : {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
    if (!ContextInfo.getPossibleNames().empty()) {
      Lookup.addArgNameCompletionResults(ContextInfo.getPossibleNames());
      break;
    }
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes());
    DoPostfixExprBeginning();
    break;
  }

  case CompletionKind::ReturnStmtExpr : {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.setExpectedTypes(getReturnTypeFromContext(CurDeclContext));
    Lookup.getValueCompletionsInDeclContext(Loc);
    break;
  }

  case CompletionKind::YieldStmtExpr: {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    if (auto FD = dyn_cast<AccessorDecl>(CurDeclContext)) {
      if (FD->isCoroutine()) {
        // TODO: handle multi-value yields.
        Lookup.setExpectedTypes(FD->getStorage()->getValueInterfaceType());
      }
    }
    Lookup.getValueCompletionsInDeclContext(Loc);
    break;
  }

  case CompletionKind::AfterPoundExpr: {
    Lookup.addPoundAvailable(ParentStmtKind);
    Lookup.addPoundSelector(/*needPound=*/false);
    Lookup.addPoundKeyPath(/*needPound=*/false);
    break;
  }

  case CompletionKind::AfterPoundDirective: {
    addPoundDirectives(CompletionContext.getResultSink());
    // FIXME: Add pound expressions (e.g. '#selector()') if it's at statements
    // position.
    break;
  }

  case CompletionKind::PlatformConditon: {
    addPlatformConditions(CompletionContext.getResultSink());
    addConditionalCompilationFlags(CurDeclContext->getASTContext(),
                                   CompletionContext.getResultSink());
    break;
  }

  case CompletionKind::GenericParams:
    if (auto GT = ParsedTypeLoc.getType()->getAnyGeneric()) {
      if (auto Params = GT->getGenericParams()) {
        for (auto GP : Params->getParams()) {
          Lookup.addGenericTypeParamRef(GP,
                                        DeclVisibilityKind::GenericParameter);
        }
      }
    }
    break;
  case CompletionKind::AfterIfStmtElse:
    // Handled earlier by keyword completions.
    break;
  case CompletionKind::PrecedenceGroup:
    Lookup.getPrecedenceGroupCompletions(SyntxKind);
    break;
  }

  for (auto &Request: Lookup.RequestedCachedResults) {
    // Use the current SourceFile as the DeclContext so that we can use it to
    // perform qualified lookup, and to get the correct visibility for
    // @testable imports.
    const SourceFile &SF = P.SF;

    llvm::DenseSet<CodeCompletionCache::Key> ImportsSeen;
    auto handleImport = [&](ModuleDecl::ImportedModule Import) {
      ModuleDecl *TheModule = Import.second;
      ModuleDecl::AccessPathTy Path = Import.first;
      if (TheModule->getFiles().empty())
        return;

      // Clang submodules are ignored and there's no lookup cost involved,
      // so just ignore them and don't put the empty results in the cache
      // because putting a lot of objects in the cache will push out
      // other lookups.
      if (isClangSubModule(TheModule))
        return;

      std::vector<std::string> AccessPath;
      for (auto Piece : Path) {
        AccessPath.push_back(Piece.first.str());
      }

      StringRef ModuleFilename = TheModule->getModuleFilename();
      // ModuleFilename can be empty if something strange happened during
      // module loading, for example, the module file is corrupted.
      if (!ModuleFilename.empty()) {
        auto &Ctx = TheModule->getASTContext();
        CodeCompletionCache::Key K{
          ModuleFilename, TheModule->getName().str(), AccessPath,
              Request.NeedLeadingDot,
              SF.hasTestableOrPrivateImport(
                  AccessLevel::Internal, TheModule,
                  SourceFile::ImportQueryKind::TestableOnly),
              SF.hasTestableOrPrivateImport(
                  AccessLevel::Internal, TheModule,
                  SourceFile::ImportQueryKind::PrivateOnly),
          Ctx.LangOpts.CodeCompleteInitsInPostfixExpr};

        using PairType = llvm::DenseSet<swift::ide::CodeCompletionCache::Key,
            llvm::DenseMapInfo<CodeCompletionCache::Key>>::iterator;
        std::pair<PairType, bool> Result = ImportsSeen.insert(K);
        if (!Result.second)
          return; // already handled.
        RequestedModules.push_back({std::move(K), TheModule,
          Request.OnlyTypes, Request.OnlyPrecedenceGroups});
      }
    };

    if (Request.TheModule) {
      // FIXME: actually check imports.
      const_cast<ModuleDecl*>(Request.TheModule)
          ->forAllVisibleModules({}, handleImport);
    } else {
      // Add results from current module.
      Lookup.getToplevelCompletions(Request.OnlyTypes);

      // Add results for all imported modules.
      SmallVector<ModuleDecl::ImportedModule, 4> Imports;
      auto *SF = CurDeclContext->getParentSourceFile();
      SF->getImportedModules(Imports, ModuleDecl::ImportFilter::All);

      for (auto Imported : Imports) {
        ModuleDecl *TheModule = Imported.second;
        ModuleDecl::AccessPathTy AccessPath = Imported.first;
        TheModule->forAllVisibleModules(AccessPath, handleImport);
      }
    }
    Lookup.RequestedCachedResults.clear();
  }

  CompletionContext.HasExpectedTypeRelation = Lookup.hasExpectedTypes();

  deliverCompletionResults();
}

void CodeCompletionCallbacksImpl::deliverCompletionResults() {
  // Use the current SourceFile as the DeclContext so that we can use it to
  // perform qualified lookup, and to get the correct visibility for
  // @testable imports.
  DeclContext *DCForModules = &P.SF;

  Consumer.handleResultsAndModules(CompletionContext, RequestedModules,
                                   DCForModules);
  RequestedModules.clear();
  DeliveredResults = true;
}

void PrintingCodeCompletionConsumer::handleResults(
    MutableArrayRef<CodeCompletionResult *> Results) {
  unsigned NumResults = 0;
  for (auto Result : Results) {
    if (!IncludeKeywords && Result->getKind() == CodeCompletionResult::Keyword)
      continue;
    NumResults++;
  }
  if (NumResults == 0)
    return;

  OS << "Begin completions, " << NumResults << " items\n";
  for (auto Result : Results) {
    if (!IncludeKeywords && Result->getKind() == CodeCompletionResult::Keyword)
      continue;
    Result->print(OS);

    llvm::SmallString<64> Name;
    llvm::raw_svector_ostream NameOs(Name);
    Result->getCompletionString()->getName(NameOs);
    OS << "; name=" << Name;

    StringRef comment = Result->getBriefDocComment();
    if (IncludeComments && !comment.empty()) {
      OS << "; comment=" << comment;
    }

    OS << "\n";
  }
  OS << "End completions\n";
}

namespace {
class CodeCompletionCallbacksFactoryImpl
    : public CodeCompletionCallbacksFactory {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;

public:
  CodeCompletionCallbacksFactoryImpl(CodeCompletionContext &CompletionContext,
                                     CodeCompletionConsumer &Consumer)
      : CompletionContext(CompletionContext), Consumer(Consumer) {}

  CodeCompletionCallbacks *createCodeCompletionCallbacks(Parser &P) override {
    return new CodeCompletionCallbacksImpl(P, CompletionContext, Consumer);
  }
};
} // end anonymous namespace

CodeCompletionCallbacksFactory *
swift::ide::makeCodeCompletionCallbacksFactory(
    CodeCompletionContext &CompletionContext,
    CodeCompletionConsumer &Consumer) {
  return new CodeCompletionCallbacksFactoryImpl(CompletionContext, Consumer);
}

void swift::ide::lookupCodeCompletionResultsFromModule(
    CodeCompletionResultSink &targetSink, const ModuleDecl *module,
    ArrayRef<std::string> accessPath, bool needLeadingDot,
    const DeclContext *currDeclContext) {
  CompletionLookup Lookup(targetSink, module->getASTContext(), currDeclContext);
  Lookup.getVisibleDeclsOfModule(module, accessPath, needLeadingDot);
}

void swift::ide::copyCodeCompletionResults(CodeCompletionResultSink &targetSink,
                                           CodeCompletionResultSink &sourceSink,
                                           bool onlyTypes,
                                           bool onlyPrecedenceGroups) {

  // We will be adding foreign results (from another sink) into TargetSink.
  // TargetSink should have an owning pointer to the allocator that keeps the
  // results alive.
  targetSink.ForeignAllocators.push_back(sourceSink.Allocator);

  if (onlyTypes) {
    std::copy_if(sourceSink.Results.begin(), sourceSink.Results.end(),
                 std::back_inserter(targetSink.Results),
                 [](CodeCompletionResult *R) -> bool {
      if (R->getKind() != CodeCompletionResult::Declaration)
        return false;
      switch(R->getAssociatedDeclKind()) {
      case CodeCompletionDeclKind::PrecedenceGroup:
      case CodeCompletionDeclKind::Module:
      case CodeCompletionDeclKind::Class:
      case CodeCompletionDeclKind::Struct:
      case CodeCompletionDeclKind::Enum:
      case CodeCompletionDeclKind::Protocol:
      case CodeCompletionDeclKind::TypeAlias:
      case CodeCompletionDeclKind::AssociatedType:
      case CodeCompletionDeclKind::GenericTypeParam:
        return true;
      case CodeCompletionDeclKind::EnumElement:
      case CodeCompletionDeclKind::Constructor:
      case CodeCompletionDeclKind::Destructor:
      case CodeCompletionDeclKind::Subscript:
      case CodeCompletionDeclKind::StaticMethod:
      case CodeCompletionDeclKind::InstanceMethod:
      case CodeCompletionDeclKind::PrefixOperatorFunction:
      case CodeCompletionDeclKind::PostfixOperatorFunction:
      case CodeCompletionDeclKind::InfixOperatorFunction:
      case CodeCompletionDeclKind::FreeFunction:
      case CodeCompletionDeclKind::StaticVar:
      case CodeCompletionDeclKind::InstanceVar:
      case CodeCompletionDeclKind::LocalVar:
      case CodeCompletionDeclKind::GlobalVar:
        return false;
      }

      llvm_unreachable("Unhandled CodeCompletionDeclKind in switch.");
    });
  } else if (onlyPrecedenceGroups) {
    std::copy_if(sourceSink.Results.begin(), sourceSink.Results.end(),
                 std::back_inserter(targetSink.Results),
                 [](CodeCompletionResult *R) -> bool {
      return R->getAssociatedDeclKind() ==
               CodeCompletionDeclKind::PrecedenceGroup;
    });
  } else {
    targetSink.Results.insert(targetSink.Results.end(),
                              sourceSink.Results.begin(),
                              sourceSink.Results.end());
  }
}

void SimpleCachingCodeCompletionConsumer::handleResultsAndModules(
    CodeCompletionContext &context,
    ArrayRef<RequestedCachedModule> requestedModules,
    DeclContext *DCForModules) {
  for (auto &R : requestedModules) {
    // FIXME(thread-safety): lock the whole AST context.  We might load a
    // module.
    llvm::Optional<CodeCompletionCache::ValueRefCntPtr> V =
        context.Cache.get(R.Key);
    if (!V.hasValue()) {
      // No cached results found. Fill the cache.
      V = context.Cache.createValue();
      lookupCodeCompletionResultsFromModule(
          (*V)->Sink, R.TheModule, R.Key.AccessPath,
          R.Key.ResultsHaveLeadingDot, DCForModules);
      context.Cache.set(R.Key, *V);
    }
    assert(V.hasValue());
    copyCodeCompletionResults(context.getResultSink(), (*V)->Sink,
                              R.OnlyTypes, R.OnlyPrecedenceGroups);
  }

  handleResults(context.takeResults());
}
