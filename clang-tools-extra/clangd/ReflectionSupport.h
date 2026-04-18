//===--- ReflectionSupport.h - P2996 AST helpers for clangd -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Centralised helpers for extracting semantic information from P2996 reflection
// AST nodes (CXXReflectExpr, CXXSpliceExpr, ReflectionSpliceType, etc.).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_REFLECTIONSUPPORT_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_REFLECTIONSUPPORT_H

#include "clang/AST/APValue.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace clang {
namespace clangd {

/// Extract the target NamedDecl from a non-dependent CXXReflectExpr.
/// Returns nullopt for dependent expressions or non-declaration reflections
/// (e.g., value reflections).
std::optional<const NamedDecl *> getReflectedDecl(const CXXReflectExpr *E);

/// Extract the QualType from a type-reflection CXXReflectExpr.
/// Returns nullopt for non-type reflections or dependent expressions.
std::optional<QualType> getReflectedType(const CXXReflectExpr *E);

/// Extract the target NamedDecl from a resolved (non-dependent) CXXSpliceExpr
/// via its model expression.
std::optional<const NamedDecl *> getSplicedDecl(const CXXSpliceExpr *E);

/// Extract the underlying type from a resolved ReflectionSpliceType.
/// Returns nullopt for dependent splice types.
std::optional<QualType> getSplicedType(const ReflectionSpliceType *T);

/// Get a human-readable description of a ReflectionKind for hover/display.
llvm::StringRef reflectionKindName(ReflectionKind K);

/// Try to extract a NamedDecl from an APValue that is a reflection.
/// Handles Type, Declaration, Template, Namespace, EntityProxy, Parameter.
std::optional<const NamedDecl *> getDeclFromReflection(const APValue &V);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_REFLECTIONSUPPORT_H
