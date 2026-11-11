#pragma once

// Single runtime-reported release identity for the CLI, broker and package.
// Keep equal to vsix/source.extension.vsixmanifest Identity Version and
// installer/Product.wxs Package Version; installer/verify_release_payload.ps1
// fails the release when they differ.
namespace bh_agent_version
{
static const char version[] = "1.0.1";
// Comma-separated capability flags. Additive only; never rename a shipped flag.
static const char features[] = "profile_output,profile_analyze_function,profile_analyze_filters,profile_analyze_subtree_scope,profile_analyze_sections,doctor_versions,mcp_stdio";
}
