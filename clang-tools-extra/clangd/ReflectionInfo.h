//===--- ReflectionInfo.h - C++ reflection inspection for clangd -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_REFLECTIONINFO_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_REFLECTIONINFO_H

#include "ParsedAST.h"
#include "Protocol.h"
#include "clang/AST/APValue.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/Type.h"
#include "llvm/Support/JSON.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace clangd {

struct ReflectionInfoIncludeOptions {
  bool Members = true;
  bool Bases = true;
  bool Annotations = true;
  bool TemplateArguments = true;
  bool Layout = true;
  bool SourceLocation = true;
};
bool fromJSON(const llvm::json::Value &, ReflectionInfoIncludeOptions &,
              llvm::json::Path);

struct ReflectionInfoParams : TextDocumentPositionParams {
  std::optional<int> MaxDepth;
  std::optional<int> MaxChildren;
  ReflectionInfoIncludeOptions Include;
};
bool fromJSON(const llvm::json::Value &, ReflectionInfoParams &,
              llvm::json::Path);

struct ReflectionInfoLayout {
  std::optional<uint64_t> Size;      // Bytes, when exactly byte-addressable.
  std::optional<uint64_t> Alignment; // Bytes, when exactly byte-addressable.
  std::optional<uint64_t> Offset;    // Bytes, when exactly byte-addressable.
  std::optional<uint64_t> SizeBits;
  std::optional<uint64_t> AlignmentBits;
  std::optional<uint64_t> OffsetBits;
};
llvm::json::Value toJSON(const ReflectionInfoLayout &);

struct ReflectionInfoDiagnostic {
  std::string Message;
};
llvm::json::Value toJSON(const ReflectionInfoDiagnostic &);

struct ReflectionInfoNode {
  std::optional<std::string> Role;
  std::string Kind;
  std::optional<std::string> Display;
  std::optional<std::string> Identifier;
  std::optional<std::string> Type;
  std::optional<std::string> Target;
  std::optional<Location> SourceLocation;
  std::optional<ReflectionInfoLayout> Layout;
  std::vector<ReflectionInfoNode> Children;
  bool Truncated = false;
  std::vector<ReflectionInfoDiagnostic> Diagnostics;
};
llvm::json::Value toJSON(const ReflectionInfoNode &);

struct ReflectionInfoOptions {
  unsigned MaxDepth = 2;
  unsigned MaxChildren = 64;
  ReflectionInfoIncludeOptions Include;
};

std::optional<ReflectionInfoNode>
getReflectionInfoForValue(const APValue &Value, QualType ValueType,
                          const ASTContext &Ctx, const PrintingPolicy &PP,
                          const ReflectionInfoOptions &Opts = {});

std::optional<std::string>
renderReflectionInfoForHover(const APValue &Value, QualType ValueType,
                             const ASTContext &Ctx, const PrintingPolicy &PP);

std::optional<ReflectionInfoNode> getReflectionInfo(ParsedAST &AST,
                                                    Position Pos,
                                                    ReflectionInfoOptions Opts);

} // namespace clangd
} // namespace clang

#endif
