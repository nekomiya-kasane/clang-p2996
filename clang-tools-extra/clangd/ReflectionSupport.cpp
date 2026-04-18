//===--- ReflectionSupport.cpp - P2996 AST helpers for clangd ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ReflectionSupport.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Reflection.h"
#include "clang/AST/SpliceSpecifier.h"

namespace clang {
namespace clangd {

std::optional<const NamedDecl *> getDeclFromReflection(const APValue &V) {
  if (!V.isReflection())
    return std::nullopt;

  switch (V.getReflectionKind()) {
  case ReflectionKind::Type: {
    QualType QT = V.getReflectedType();
    if (const auto *TT = QT->getAs<TagType>())
      return TT->getDecl();
    if (const auto *TDT = QT->getAs<TypedefType>())
      return TDT->getDecl();
    if (const auto *TTPT = QT->getAs<TemplateTypeParmType>())
      return TTPT->getDecl();
    return std::nullopt;
  }
  case ReflectionKind::Declaration: {
    ValueDecl *D = V.getReflectedDecl();
    if (auto *ND = dyn_cast_or_null<NamedDecl>(D))
      return ND;
    return std::nullopt;
  }
  case ReflectionKind::Template: {
    TemplateName TN = V.getReflectedTemplate();
    if (auto *TD = TN.getAsTemplateDecl())
      return TD;
    return std::nullopt;
  }
  case ReflectionKind::Namespace: {
    Decl *D = V.getReflectedNamespace();
    if (auto *ND = dyn_cast_or_null<NamedDecl>(D))
      return ND;
    return std::nullopt;
  }
  case ReflectionKind::EntityProxy: {
    auto *USD = V.getReflectedEntityProxy();
    if (USD)
      return USD;
    return std::nullopt;
  }
  case ReflectionKind::Parameter: {
    auto *PVD = V.getReflectedParameter();
    if (PVD)
      return PVD;
    return std::nullopt;
  }
  case ReflectionKind::Null:
  case ReflectionKind::Object:
  case ReflectionKind::Value:
  case ReflectionKind::BaseSpecifier:
  case ReflectionKind::DataMemberSpec:
  case ReflectionKind::Annotation:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<const NamedDecl *> getReflectedDecl(const CXXReflectExpr *E) {
  if (!E || E->hasDependentSubExpr())
    return std::nullopt;
  return getDeclFromReflection(E->getReflection());
}

std::optional<QualType> getReflectedType(const CXXReflectExpr *E) {
  if (!E || E->hasDependentSubExpr())
    return std::nullopt;
  APValue V = E->getReflection();
  if (!V.isReflection() || V.getReflectionKind() != ReflectionKind::Type)
    return std::nullopt;
  return V.getReflectedType();
}

std::optional<const NamedDecl *> getSplicedDecl(const CXXSpliceExpr *E) {
  if (!E)
    return std::nullopt;
  // The model expression is the resolved expression that the splice evaluates
  // to. If it's a DeclRefExpr we can extract the target declaration.
  const Expr *Model = E->getModel();
  if (!Model)
    return std::nullopt;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Model))
    return DRE->getFoundDecl();
  if (const auto *ME = dyn_cast<MemberExpr>(Model))
    return ME->getFoundDecl().getDecl();
  return std::nullopt;
}

std::optional<QualType> getSplicedType(const ReflectionSpliceType *T) {
  if (!T || T->isDependentType())
    return std::nullopt;
  return T->desugar();
}

llvm::StringRef reflectionKindName(ReflectionKind K) {
  switch (K) {
  case ReflectionKind::Null: return "null";
  case ReflectionKind::Type: return "type";
  case ReflectionKind::Object: return "object";
  case ReflectionKind::Value: return "value";
  case ReflectionKind::Declaration: return "declaration";
  case ReflectionKind::Template: return "template";
  case ReflectionKind::Namespace: return "namespace";
  case ReflectionKind::EntityProxy: return "entity proxy";
  case ReflectionKind::Parameter: return "parameter";
  case ReflectionKind::BaseSpecifier: return "base specifier";
  case ReflectionKind::DataMemberSpec: return "data member spec";
  case ReflectionKind::Annotation: return "annotation";
  }
  return "unknown";
}

} // namespace clangd
} // namespace clang
