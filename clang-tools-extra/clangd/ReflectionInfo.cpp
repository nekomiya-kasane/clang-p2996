//===--- ReflectionInfo.cpp - C++ reflection inspection for clangd --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ReflectionInfo.h"
#include "AST.h"
#include "Selection.h"
#include "SourceCode.h"
#include "URI.h"
#include "clang/AST/APValue.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Reflection.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Lex/Lexer.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace clang {
namespace clangd {
namespace {

constexpr unsigned DefaultMaxDepth = 2;
constexpr unsigned DefaultMaxChildren = 64;
constexpr unsigned MaximumMaxDepth = 8;
constexpr unsigned MaximumMaxChildren = 512;

ReflectionInfoOptions normalize(ReflectionInfoOptions Opts) {
  Opts.MaxDepth = std::min(Opts.MaxDepth, MaximumMaxDepth);
  Opts.MaxChildren = std::min(Opts.MaxChildren, MaximumMaxChildren);
  return Opts;
}

PrintingPolicy reflectionPrintingPolicy(PrintingPolicy Base) {
  Base.AnonymousTagLocations = false;
  Base.TerseOutput = true;
  Base.PolishForDeclaration = true;
  Base.ConstantsAsWritten = true;
  Base.SuppressTemplateArgsInCXXConstructors = true;
  return Base;
}
std::string printTypeText(QualType QT, const PrintingPolicy &PP) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  if (QT.isNull())
    OS << "<unknown>";
  else
    QT.print(OS, PP);
  return Result;
}

std::string printDeclName(const NamedDecl *ND) {
  if (!ND)
    return "<unknown>";
  if (ND->getDeclName().isEmpty())
    return "<anonymous>";
  return printQualifiedName(*ND);
}

std::string printDeclTarget(const Decl *D, const PrintingPolicy &PP) {
  if (!D)
    return "<unknown>";
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  D->print(OS, PP);
  return Result;
}

std::string printExprText(const Expr *E, const PrintingPolicy &PP) {
  if (!E)
    return "<unknown>";
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  E->printPretty(OS, nullptr, PP);
  return Result;
}

std::string printTemplateArgument(const TemplateArgument &Arg,
                                  const PrintingPolicy &PP) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  Arg.print(PP, OS, /*IncludeType=*/true);
  return Result;
}

std::optional<std::string> identifierForReflectedType(QualType QT) {
  if (QT.isNull())
    return std::nullopt;
  if (const auto *TT = QT->getAs<TagType>()) {
    const TagDecl *TD = TT->getDecl();
    if (TD && !TD->getName().empty())
      return TD->getNameAsString();
  }
  if (const auto *TST = QT->getAs<TemplateSpecializationType>()) {
    if (TemplateDecl *TD = TST->getTemplateName().getAsTemplateDecl())
      if (!TD->getName().empty())
        return TD->getNameAsString();
  }
  if (const auto *TD = QT->getAs<TypedefType>())
    if (!TD->getDecl()->getName().empty())
      return TD->getDecl()->getNameAsString();
  return std::nullopt;
}

const char *reflectionKindName(ReflectionKind Kind) {
  switch (Kind) {
  case ReflectionKind::Null:
    return "null";
  case ReflectionKind::Type:
    return "type";
  case ReflectionKind::Object:
    return "object";
  case ReflectionKind::Value:
    return "value";
  case ReflectionKind::Declaration:
    return "declaration";
  case ReflectionKind::Template:
    return "template";
  case ReflectionKind::Namespace:
    return "namespace";
  case ReflectionKind::EntityProxy:
    return "entity-proxy";
  case ReflectionKind::Parameter:
    return "parameter";
  case ReflectionKind::BaseSpecifier:
    return "base-specifier";
  case ReflectionKind::DataMemberSpec:
    return "data-member-spec";
  case ReflectionKind::Annotation:
    return "annotation";
  case ReflectionKind::EnumeratorSpec:
    return "enumerator-spec";
  case ReflectionKind::Attribute:
    return "attribute";
  }
  llvm_unreachable("unknown reflection kind");
}

PathRef mainFilePath(const SourceManager &SM) {
  if (auto FE = SM.getFileEntryRefForID(SM.getMainFileID()))
    return FE->getName();
  return "";
}

std::optional<Location>
sourceRangeLocation(SourceRange SR, const ASTContext &Ctx, PathRef TUPath) {
  const auto &SM = Ctx.getSourceManager();
  if (auto FileRange = toHalfOpenFileRange(SM, Ctx.getLangOpts(), SR)) {
    SourceLocation Loc = FileRange->getBegin();
    if (!Loc.isValid() || !Loc.isFileID())
      return std::nullopt;
    auto File = SM.getFileEntryRefForID(SM.getFileID(Loc));
    if (!File)
      return std::nullopt;
    return Location{
        URIForFile::canonicalize(File->getName(), TUPath),
        halfOpenToRange(SM, CharSourceRange::getCharRange(*FileRange))};
  }

  SourceLocation Loc = SM.getExpansionLoc(SR.getBegin());
  if (!Loc.isValid() || !Loc.isFileID())
    return std::nullopt;
  auto File = SM.getFileEntryRefForID(SM.getFileID(Loc));
  if (!File)
    return std::nullopt;
  Range R;
  R.start = sourceLocToPosition(SM, Loc);
  SourceLocation End = SM.getExpansionLoc(SR.getEnd());
  if (End.isValid() && End.isFileID() && SM.getFileID(End) == SM.getFileID(Loc))
    R.end = sourceLocToPosition(SM, End);
  else
    R.end = R.start;
  return Location{URIForFile::canonicalize(File->getName(), TUPath), R};
}

std::optional<Location> declLocation(const Decl *D, const ASTContext &Ctx,
                                     PathRef TUPath) {
  if (!D)
    return std::nullopt;
  return sourceRangeLocation(D->getSourceRange(), Ctx, TUPath);
}

std::optional<Location> attrLocation(const AttributeCommonInfo *A,
                                     const ASTContext &Ctx, PathRef TUPath) {
  if (!A)
    return std::nullopt;
  return sourceRangeLocation(A->getRange(), Ctx, TUPath);
}

void setByteValueIfExact(std::optional<uint64_t> &Bytes, uint64_t Bits,
                         uint64_t CharWidth) {
  if (CharWidth != 0 && Bits % CharWidth == 0)
    Bytes = Bits / CharWidth;
}

std::optional<ReflectionInfoLayout> layoutForType(QualType QT,
                                                  const ASTContext &Ctx) {
  if (QT.isNull() || QT->isDependentType() || QT->isIncompleteType())
    return std::nullopt;
  ReflectionInfoLayout Layout;
  Layout.SizeBits = Ctx.getTypeSize(QT);
  Layout.AlignmentBits = Ctx.getTypeAlign(QT);
  setByteValueIfExact(Layout.Size, *Layout.SizeBits, Ctx.getCharWidth());
  setByteValueIfExact(Layout.Alignment, *Layout.AlignmentBits,
                      Ctx.getCharWidth());
  return Layout;
}

std::optional<ReflectionInfoLayout> layoutForField(const FieldDecl *FD,
                                                   const ASTContext &Ctx) {
  if (!FD)
    return std::nullopt;
  ReflectionInfoLayout Layout = layoutForType(FD->getType(), Ctx).value_or({});
  if (FD->isBitField()) {
    Layout.Size.reset();
    Layout.SizeBits = FD->getBitWidthValue();
  }
  const auto *Parent = dyn_cast<CXXRecordDecl>(FD->getParent());
  if (Parent && Parent->isCompleteDefinition()) {
    Layout.OffsetBits =
        Ctx.getASTRecordLayout(Parent).getFieldOffset(FD->getFieldIndex());
    setByteValueIfExact(Layout.Offset, *Layout.OffsetBits, Ctx.getCharWidth());
  }
  return Layout;
}

std::optional<ReflectionInfoLayout> layoutForBase(const CXXBaseSpecifier &Base,
                                                  const ASTContext &Ctx) {
  const auto *Derived = Base.getDerived();
  const auto *BaseRecord = Base.getType()->getAsCXXRecordDecl();
  if (!Derived || !BaseRecord || !Derived->isCompleteDefinition())
    return std::nullopt;
  const auto &Layout = Ctx.getASTRecordLayout(Derived);
  ReflectionInfoLayout Result = layoutForType(Base.getType(), Ctx).value_or({});
  Result.OffsetBits = static_cast<uint64_t>(
      Ctx.toBits(Base.isVirtual() ? Layout.getVBaseClassOffset(BaseRecord)
                                  : Layout.getBaseClassOffset(BaseRecord)));
  setByteValueIfExact(Result.Offset, *Result.OffsetBits, Ctx.getCharWidth());
  return Result;
}

struct BudgetedBuilder {
  const ASTContext &Ctx;
  const PrintingPolicy &PP;
  const ReflectionInfoOptions &Opts;
  PathRef TUPath;

  bool addChild(ReflectionInfoNode &Parent, ReflectionInfoNode Child) const {
    if (Parent.Children.size() >= Opts.MaxChildren) {
      Parent.Truncated = true;
      return false;
    }
    Parent.Children.push_back(std::move(Child));
    return true;
  }

  ReflectionInfoNode fromDecl(const NamedDecl *ND, llvm::StringRef Role) const {
    ReflectionInfoNode Node;
    if (!Role.empty())
      Node.Role = Role.str();
    Node.Kind = "declaration";
    Node.Display = printDeclName(ND);
    if (ND && !ND->getName().empty())
      Node.Identifier = ND->getNameAsString();
    if (const auto *VD = dyn_cast_or_null<ValueDecl>(ND))
      Node.Type = printTypeText(VD->getType(), PP);
    Node.Target = printDeclTarget(ND, PP);
    if (Opts.Include.SourceLocation)
      Node.SourceLocation = declLocation(ND, Ctx, TUPath);
    if (Opts.Include.Layout)
      if (const auto *FD = dyn_cast_or_null<FieldDecl>(ND))
        Node.Layout = layoutForField(FD, Ctx);
    return Node;
  }

  ReflectionInfoNode fromType(QualType QT, llvm::StringRef Role,
                              unsigned Depth) const {
    ReflectionInfoNode Node;
    if (!Role.empty())
      Node.Role = Role.str();
    Node.Kind = "type";
    std::string Type = printTypeText(QT, PP);
    Node.Display = Type;
    Node.Identifier = identifierForReflectedType(QT);
    Node.Type = std::move(Type);
    if (Opts.Include.Layout)
      Node.Layout = layoutForType(QT, Ctx);
    if (Opts.Include.SourceLocation) {
      if (const TagDecl *TD = QT->getAsTagDecl())
        Node.SourceLocation = declLocation(TD, Ctx, TUPath);
    }
    addTypeTemplateArguments(Node, QT);
    addTypeChildren(Node, QT, Depth);
    return Node;
  }

  ReflectionInfoNode fromAnnotation(const CXX26AnnotationAttr *A,
                                    llvm::StringRef Role) const {
    ReflectionInfoNode Node;
    if (!Role.empty())
      Node.Role = Role.str();
    Node.Kind = "annotation";
    if (A && A->getArg()) {
      Node.Display = printExprText(A->getArg(), PP);
      Node.Target = *Node.Display;
      Node.Type = printTypeText(A->getArg()->getType(), PP);
    } else {
      Node.Display = "annotation";
    }
    if (Opts.Include.SourceLocation)
      Node.SourceLocation = attrLocation(A, Ctx, TUPath);
    return Node;
  }

  ReflectionInfoNode fromBase(const CXXBaseSpecifier &Base,
                              llvm::StringRef Role) const {
    ReflectionInfoNode Node;
    if (!Role.empty())
      Node.Role = Role.str();
    Node.Kind = "base-specifier";
    Node.Display = printTypeText(Base.getType(), PP);
    Node.Type = *Node.Display;
    std::string Target;
    llvm::raw_string_ostream OS(Target);
    if (StringRef Access = getAccessSpelling(Base.getAccessSpecifier());
        !Access.empty())
      OS << Access << " ";
    if (Base.isVirtual())
      OS << "virtual ";
    OS << *Node.Display;
    Node.Target = std::move(Target);
    if (Opts.Include.SourceLocation)
      Node.SourceLocation =
          sourceRangeLocation(Base.getSourceRange(), Ctx, TUPath);
    if (Opts.Include.Layout)
      Node.Layout = layoutForBase(Base, Ctx);
    return Node;
  }

  ReflectionInfoNode fromTemplateArgument(const TemplateArgument &Arg,
                                          llvm::StringRef Role) const {
    ReflectionInfoNode Node;
    if (!Role.empty())
      Node.Role = Role.str();
    Node.Kind = "template-argument";
    Node.Display = printTemplateArgument(Arg, PP);
    if (Arg.getKind() == TemplateArgument::Type)
      Node.Type = printTypeText(Arg.getAsType(), PP);
    if (Arg.getKind() == TemplateArgument::Integral &&
        !Arg.getIntegralType().isNull())
      Node.Type = printTypeText(Arg.getIntegralType(), PP);
    if (Arg.getKind() == TemplateArgument::Declaration && Arg.getAsDecl())
      Node.Target = printDeclTarget(Arg.getAsDecl(), PP);
    return Node;
  }

  void addDeclAnnotations(ReflectionInfoNode &Node, const Decl *D) const {
    if (!D || !Opts.Include.Annotations)
      return;
    for (const auto *A : D->specific_attrs<CXX26AnnotationAttr>())
      if (!addChild(Node, fromAnnotation(A, "annotation")))
        return;
  }

  void addTemplateArgument(ReflectionInfoNode &Node,
                           const TemplateArgument &Arg) const {
    if (Arg.getKind() == TemplateArgument::Pack) {
      for (const TemplateArgument &PackArg : Arg.getPackAsArray())
        if (!addChild(Node, fromTemplateArgument(PackArg, "template-argument")))
          return;
      return;
    }
    addChild(Node, fromTemplateArgument(Arg, "template-argument"));
  }

  void addTemplateArguments(ReflectionInfoNode &Node,
                            const TemplateArgumentList *Args) const {
    if (!Args)
      return;
    addTemplateArguments(Node, Args->asArray());
  }

  void addTemplateArguments(ReflectionInfoNode &Node,
                            llvm::ArrayRef<TemplateArgument> Args) const {
    if (!Opts.Include.TemplateArguments)
      return;
    for (const TemplateArgument &Arg : Args)
      addTemplateArgument(Node, Arg);
  }

  void addDeclarationTemplateArguments(ReflectionInfoNode &Node,
                                       const ValueDecl *D) const {
    if (!D || !Opts.Include.TemplateArguments)
      return;
    if (const auto *FD = dyn_cast<FunctionDecl>(D))
      addTemplateArguments(Node, FD->getTemplateSpecializationArgs());
    else if (const auto *VD = dyn_cast<VarTemplateSpecializationDecl>(D))
      addTemplateArguments(Node, &VD->getTemplateArgs());
  }

  void addTypeTemplateArguments(ReflectionInfoNode &Node, QualType QT) const {
    if (QT.isNull() || !Opts.Include.TemplateArguments)
      return;
    if (const auto *TST = QT->getAs<TemplateSpecializationType>())
      addTemplateArguments(Node, TST->template_arguments());
    else if (const auto *DTST =
                 QT->getAs<DependentTemplateSpecializationType>())
      addTemplateArguments(Node, DTST->template_arguments());
    else if (const auto *CTS =
                 dyn_cast_or_null<ClassTemplateSpecializationDecl>(
                     QT->getAsRecordDecl()))
      addTemplateArguments(Node, CTS->getTemplateArgs().asArray());
  }

  void addFunctionChildren(ReflectionInfoNode &Node,
                           const FunctionDecl *FD) const {
    if (!FD)
      return;
    ReflectionInfoNode Return;
    Return.Role = "return-type";
    Return.Kind = "type";
    Return.Display = printTypeText(FD->getReturnType(), PP);
    Return.Type = *Return.Display;
    if (!addChild(Node, std::move(Return)) || !Opts.Include.Members)
      return;
    for (const ParmVarDecl *P : FD->parameters())
      if (!addChild(Node, fromDecl(P, "parameter")))
        break;
  }

  void addRecordChildren(ReflectionInfoNode &Node,
                         const CXXRecordDecl *RD) const {
    if (!RD)
      return;
    const CXXRecordDecl *Definition = RD->getDefinition();
    addDeclAnnotations(Node, Definition ? Definition : RD);
    if (!Definition)
      return;
    RD = Definition;
    if (const auto *CTS = dyn_cast<ClassTemplateSpecializationDecl>(RD);
        CTS && std::none_of(Node.Children.begin(), Node.Children.end(),
                            [](const ReflectionInfoNode &Child) {
                              return Child.Role == "template-argument";
                            }))
      addTemplateArguments(Node, &CTS->getTemplateArgs());
    if (Opts.Include.Bases) {
      for (const CXXBaseSpecifier &Base : RD->bases())
        if (!addChild(Node, fromBase(Base, "base")))
          break;
    }
    if (Opts.Include.Members) {
      for (const FieldDecl *FD : RD->fields())
        if (!addChild(Node, fromDecl(FD, "member")))
          break;
      for (const CXXMethodDecl *MD : RD->methods())
        if (!addChild(Node, fromDecl(MD, "member")))
          break;
    }
  }

  void addEnumChildren(ReflectionInfoNode &Node, const EnumDecl *ED) const {
    if (!ED)
      return;
    const EnumDecl *Definition = ED->getDefinition();
    addDeclAnnotations(Node, Definition ? Definition : ED);
    if (!Definition)
      return;
    ED = Definition;
    if (!Opts.Include.Members)
      return;
    for (const EnumConstantDecl *ECD : ED->enumerators())
      if (!addChild(Node, fromDecl(ECD, "enumerator")))
        break;
  }

  void addNamespaceChildren(ReflectionInfoNode &Node,
                            const DeclContext *DC) const {
    if (!DC || !Opts.Include.Members)
      return;
    for (const Decl *D : DC->decls()) {
      const auto *ND = dyn_cast<NamedDecl>(D);
      if (!ND || ND->isImplicit())
        continue;
      if (!addChild(Node, fromDecl(ND, "member")))
        break;
    }
  }

  void addDeclarationChildren(ReflectionInfoNode &Node,
                              const ValueDecl *D) const {
    if (!D)
      return;
    addDeclAnnotations(Node, D);
    addDeclarationTemplateArguments(Node, D);
    if (const auto *FD = dyn_cast<FunctionDecl>(D))
      addFunctionChildren(Node, FD);
  }

  void addTypeChildren(ReflectionInfoNode &Node, QualType QT,
                       unsigned Depth) const {
    if (Depth >= Opts.MaxDepth) {
      if ((QT->getAsCXXRecordDecl() &&
           (Opts.Include.Members || Opts.Include.Bases)) ||
          (QT->isEnumeralType() && Opts.Include.Members))
        Node.Truncated = true;
      return;
    }
    if (const auto *RD = QT->getAsCXXRecordDecl()) {
      addRecordChildren(Node, RD);
      return;
    }
    if (const auto *ET = QT->getAs<EnumType>()) {
      addEnumChildren(Node, ET->getDecl());
      return;
    }
    if (const TagDecl *TD = QT->getAsTagDecl())
      addDeclAnnotations(Node, TD);
  }

  ReflectionInfoNode fromReflectionValue(const APValue &Value,
                                         QualType ValueType,
                                         unsigned Depth) const {
    ReflectionInfoNode Info;
    Info.Kind = reflectionKindName(Value.getReflectionKind());
    switch (Value.getReflectionKind()) {
    case ReflectionKind::Null:
      Info.Display = "null reflection";
      break;
    case ReflectionKind::Type:
      return fromType(Value.getReflectedType(), "", Depth);
    case ReflectionKind::Object:
    case ReflectionKind::Value: {
      QualType Type = Value.getTypeOfReflectedResult(Ctx);
      Info.Type = printTypeText(Type, PP);
      APValue Reflected = Value.getReflectionKind() == ReflectionKind::Object
                              ? Value.getReflectedObject()
                              : Value.getReflectedValue();
      if (Reflected.hasValue() && !Reflected.isStruct() && !Reflected.isUnion())
        Info.Target = Reflected.getAsString(Ctx, Type);
      if (!ValueType.isNull() &&
          (Opts.Include.Members || Opts.Include.TemplateArguments))
        Info.Diagnostics.push_back(
            {llvm::formatv("value-type: {0}", printTypeText(ValueType, PP))
                 .str()});
      break;
    }
    case ReflectionKind::Declaration: {
      const ValueDecl *D = Value.getReflectedDecl();
      Info = fromDecl(D, "");
      addDeclarationChildren(Info, D);
      break;
    }
    case ReflectionKind::Template: {
      TemplateName TN = Value.getReflectedTemplate();
      std::string Name;
      llvm::raw_string_ostream OS(Name);
      TN.print(OS, PP, TemplateName::Qualified::None);
      Info.Display = Name.empty() ? std::string("<template>") : Name;
      if (TemplateDecl *TD = TN.getAsTemplateDecl()) {
        Info.Identifier = TD->getNameAsString();
        Info.Target = printDeclTarget(TD, PP);
        if (Opts.Include.SourceLocation)
          Info.SourceLocation = declLocation(TD, Ctx, TUPath);
      }
      break;
    }
    case ReflectionKind::Namespace: {
      const Decl *D = Value.getReflectedNamespace();
      if (isa_and_nonnull<TranslationUnitDecl>(D)) {
        Info.Display = "<global namespace>";
        Info.Target = "<global namespace>";
      } else if (const auto *ND = dyn_cast_or_null<NamedDecl>(D)) {
        Info.Display = printDeclName(ND);
        if (!ND->getName().empty())
          Info.Identifier = ND->getNameAsString();
        Info.Target = printDeclTarget(ND, PP);
        if (Opts.Include.SourceLocation)
          Info.SourceLocation = declLocation(ND, Ctx, TUPath);
      }
      addNamespaceChildren(Info, dyn_cast_or_null<DeclContext>(D));
      break;
    }
    case ReflectionKind::EntityProxy: {
      const UsingShadowDecl *USD = Value.getReflectedEntityProxy();
      const NamedDecl *Target = USD ? USD->getTargetDecl() : nullptr;
      Info.Display = printDeclName(Target);
      if (Target && !Target->getName().empty())
        Info.Identifier = Target->getNameAsString();
      Info.Target = printDeclTarget(Target, PP);
      if (Opts.Include.SourceLocation)
        Info.SourceLocation = declLocation(Target, Ctx, TUPath);
      break;
    }
    case ReflectionKind::Parameter: {
      const ParmVarDecl *PVD = Value.getReflectedParameter();
      Info = fromDecl(PVD, "");
      Info.Kind = "parameter";
      break;
    }
    case ReflectionKind::BaseSpecifier:
      if (const CXXBaseSpecifier *Base = Value.getReflectedBaseSpecifier())
        Info = fromBase(*Base, "");
      break;
    case ReflectionKind::DataMemberSpec: {
      const TagDataMemberSpec *Spec = Value.getReflectedDataMemberSpec();
      if (Spec) {
        Info.Type = printTypeText(Spec->Ty, PP);
        Info.Display = Spec->Name ? *Spec->Name : *Info.Type;
        if (Spec->Name)
          Info.Identifier = *Spec->Name;
        if (Spec->Alignment)
          Info.Diagnostics.push_back(
              {llvm::formatv("alignment: {0}", *Spec->Alignment).str()});
        if (Spec->BitWidth)
          Info.Diagnostics.push_back(
              {llvm::formatv("bit-width: {0}", *Spec->BitWidth).str()});
        if (Spec->NoUniqueAddress)
          Info.Diagnostics.push_back({"no-unique-address: true"});
      }
      break;
    }
    case ReflectionKind::Annotation:
      Info = fromAnnotation(Value.getReflectedAnnotation(), "");
      break;
    case ReflectionKind::EnumeratorSpec: {
      const EnumeratorSpec *Spec = Value.getReflectedEnumeratorSpec();
      if (Spec) {
        Info.Display = Spec->name;
        Info.Identifier = Spec->name;
        if (Spec->hasValue)
          Info.Target = llvm::itostr(Spec->val);
        if (!Spec->annotations.empty())
          Info.Diagnostics.push_back(
              {llvm::formatv("annotations: {0}", Spec->annotations.size())
                   .str()});
        if (!Spec->attributes.empty())
          Info.Diagnostics.push_back(
              {llvm::formatv("attributes: {0}", Spec->attributes.size())
                   .str()});
      }
      break;
    }
    case ReflectionKind::Attribute: {
      const ParsedAttr *A = Value.getReflectedAttribute();
      Info.Display = A ? A->getNormalizedFullName() : "attribute";
      Info.Target = Info.Display;
      if (Opts.Include.SourceLocation)
        Info.SourceLocation = attrLocation(A, Ctx, TUPath);
      break;
    }
    }
    return Info;
  }
};

const Expr *ignoreReflectionValueNoise(const Expr *E) {
  if (!E)
    return nullptr;
  while (true) {
    E = E->IgnoreImplicit();
    if (const auto *Cleanups = dyn_cast<ExprWithCleanups>(E)) {
      E = Cleanups->getSubExpr();
      continue;
    }
    if (const auto *Materialized = dyn_cast<MaterializeTemporaryExpr>(E)) {
      E = Materialized->getSubExpr();
      continue;
    }
    if (const auto *Bound = dyn_cast<CXXBindTemporaryExpr>(E)) {
      E = Bound->getSubExpr();
      continue;
    }
    return E;
  }
}

const Expr *singleExpansionElementExpr(const Expr *E, unsigned Depth = 0) {
  if (!E || Depth > 4)
    return nullptr;
  E = ignoreReflectionValueNoise(E);

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (VD && isa<ExpansionStmtDecl>(VD->getDeclContext()))
      return singleExpansionElementExpr(VD->getInit(), Depth + 1);
  }

  if (const auto *Select = dyn_cast<CXXExpansionInitListSelectExpr>(E)) {
    const auto *Range = dyn_cast_or_null<CXXExpansionInitListExpr>(
        ignoreReflectionValueNoise(Select->getRangeExpr()));
    if (!Range || Range->getSubExprs().size() != 1)
      return nullptr;
    return ignoreReflectionValueNoise(Range->getSubExprs().front());
  }

  return nullptr;
}

std::optional<APValue> evaluateReflectionExpr(const Expr *E,
                                              const ASTContext &Ctx) {
  if (!E)
    return std::nullopt;
  if (const auto *ILE = dyn_cast<InitListExpr>(E)) {
    if (!ILE->isSemanticForm())
      E = ILE->getSemanticForm();
  }

  QualType T = E->getType();
  if (T.isNull() || !T->isReflectionType() || E->isValueDependent())
    return std::nullopt;

  if (const Expr *SingleExpansionElement = singleExpansionElementExpr(E))
    if (auto Value = evaluateReflectionExpr(SingleExpansionElement, Ctx))
      return Value;

  Expr::EvalResult Constant;
  if (!E->EvaluateAsRValue(Constant, Ctx, /*InConstantContext=*/true) ||
      !Constant.Val.isReflection())
    return std::nullopt;
  return std::move(Constant.Val);
}

std::optional<std::pair<APValue, QualType>> reflectionFromDecl(const Decl *D) {
  const auto *Var = dyn_cast_or_null<VarDecl>(D);
  if (!Var || Var->isInvalidDecl() || Var->getType().isNull() ||
      !Var->getType()->isReflectionType())
    return std::nullopt;
  if (const Expr *Init = Var->getInit())
    if (!Init->isValueDependent())
      if (const APValue *Value = Var->evaluateValue())
        if (Value->isReflection())
          return std::make_pair(*Value, Var->getType());
  return std::nullopt;
}

std::optional<std::pair<APValue, QualType>>
reflectionFromSelection(const SelectionTree::Node *N, const ASTContext &Ctx) {
  for (; N; N = N->Parent) {
    if (const Expr *E = N->ASTNode.get<Expr>()) {
      if (!E->getType().isNull() && E->getType()->isVoidType())
        break;
      if (auto Value = evaluateReflectionExpr(E, Ctx))
        return std::make_pair(std::move(*Value), E->getType());
    } else if (const Decl *D = N->ASTNode.get<Decl>()) {
      if (auto Value = reflectionFromDecl(D))
        return Value;
      break;
    } else if (N->ASTNode.get<Stmt>()) {
      break;
    }
  }
  return std::nullopt;
}

std::string renderHover(const ReflectionInfoNode &Info) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  OS << "kind: " << Info.Kind;
  if (Info.Display)
    OS << "\ndisplay: " << *Info.Display;
  if (Info.Identifier)
    OS << "\nidentifier: " << *Info.Identifier;
  if (Info.Type)
    OS << "\ntype: " << *Info.Type;
  if (Info.Target)
    OS << "\ntarget: " << *Info.Target;
  for (const auto &Diagnostic : Info.Diagnostics)
    OS << "\n" << Diagnostic.Message;
  if (Info.Truncated)
    OS << "\ntruncated: true";
  return Result;
}

} // namespace

bool fromJSON(const llvm::json::Value &Params, ReflectionInfoIncludeOptions &R,
              llvm::json::Path P) {
  llvm::json::ObjectMapper O(Params, P);
  return O && O.mapOptional("members", R.Members) &&
         O.mapOptional("bases", R.Bases) &&
         O.mapOptional("annotations", R.Annotations) &&
         O.mapOptional("templateArguments", R.TemplateArguments) &&
         O.mapOptional("layout", R.Layout) &&
         O.mapOptional("sourceLocation", R.SourceLocation);
}

bool fromJSON(const llvm::json::Value &Params, ReflectionInfoParams &R,
              llvm::json::Path P) {
  llvm::json::ObjectMapper O(Params, P);
  return fromJSON(Params, static_cast<TextDocumentPositionParams &>(R), P) &&
         O && O.mapOptional("maxDepth", R.MaxDepth) &&
         O.mapOptional("maxChildren", R.MaxChildren) &&
         O.mapOptional("include", R.Include);
}

llvm::json::Value toJSON(const ReflectionInfoLayout &L) {
  llvm::json::Object Result;
  if (L.Size)
    Result["size"] = *L.Size;
  if (L.Alignment)
    Result["alignment"] = *L.Alignment;
  if (L.Offset)
    Result["offset"] = *L.Offset;
  if (L.SizeBits)
    Result["sizeBits"] = *L.SizeBits;
  if (L.AlignmentBits)
    Result["alignmentBits"] = *L.AlignmentBits;
  if (L.OffsetBits)
    Result["offsetBits"] = *L.OffsetBits;
  return Result;
}

llvm::json::Value toJSON(const ReflectionInfoDiagnostic &D) {
  return llvm::json::Object{{"message", D.Message}};
}

llvm::json::Value toJSON(const ReflectionInfoNode &N) {
  llvm::json::Object Result{{"kind", N.Kind}};
  if (N.Role)
    Result["role"] = *N.Role;
  if (N.Display)
    Result["display"] = *N.Display;
  if (N.Identifier)
    Result["identifier"] = *N.Identifier;
  if (N.Type)
    Result["type"] = *N.Type;
  if (N.Target)
    Result["target"] = *N.Target;
  if (N.SourceLocation)
    Result["sourceLocation"] = *N.SourceLocation;
  if (N.Layout)
    Result["layout"] = *N.Layout;
  if (!N.Children.empty())
    Result["children"] = llvm::json::Array(N.Children);
  if (N.Truncated)
    Result["truncated"] = true;
  if (!N.Diagnostics.empty())
    Result["diagnostics"] = llvm::json::Array(N.Diagnostics);
  return Result;
}

std::optional<ReflectionInfoNode>
getReflectionInfoForValue(const APValue &Value, QualType ValueType,
                          const ASTContext &Ctx, const PrintingPolicy &PP,
                          const ReflectionInfoOptions &Opts) {
  if (!Value.isReflection())
    return std::nullopt;
  ReflectionInfoOptions Normalized = normalize(Opts);
  BudgetedBuilder Builder{Ctx, PP, Normalized,
                          mainFilePath(Ctx.getSourceManager())};
  return Builder.fromReflectionValue(Value, ValueType, /*Depth=*/0);
}

std::optional<std::string>
renderReflectionInfoForHover(const APValue &Value, QualType ValueType,
                             const ASTContext &Ctx, const PrintingPolicy &PP) {
  ReflectionInfoOptions Opts;
  Opts.MaxDepth = 0;
  Opts.MaxChildren = 0;
  Opts.Include.Members = false;
  Opts.Include.Bases = false;
  Opts.Include.Annotations = false;
  Opts.Include.TemplateArguments = false;
  Opts.Include.Layout = false;
  Opts.Include.SourceLocation = false;
  if (auto Info = getReflectionInfoForValue(Value, ValueType, Ctx, PP, Opts))
    return renderHover(*Info);
  return std::nullopt;
}

std::optional<ReflectionInfoNode>
getReflectionInfo(ParsedAST &AST, Position Pos, ReflectionInfoOptions Opts) {
  auto &SM = AST.getSourceManager();
  auto CurLoc = sourceLocationInMainFile(SM, Pos);
  if (!CurLoc)
    return std::nullopt;
  auto Offset = SM.getFileOffset(*CurLoc);
  SelectionTree ST = SelectionTree::createRight(
      AST.getASTContext(), AST.getTokens(), Offset, Offset);
  const SelectionTree::Node *N = ST.commonAncestor();
  if (!N)
    return std::nullopt;

  if (auto Reflection = reflectionFromSelection(N, AST.getASTContext())) {
    PrintingPolicy PP =
        reflectionPrintingPolicy(AST.getASTContext().getPrintingPolicy());
    ReflectionInfoOptions Normalized = normalize(Opts);
    BudgetedBuilder Builder{AST.getASTContext(), PP, Normalized, AST.tuPath()};
    return Builder.fromReflectionValue(Reflection->first, Reflection->second,
                                       /*Depth=*/0);
  }
  return std::nullopt;
}

} // namespace clangd
} // namespace clang
