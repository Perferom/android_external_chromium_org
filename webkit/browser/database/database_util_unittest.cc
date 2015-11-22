// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/strings/string_piece.h"
#include "base/strings/utf_string_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "webkit/browser/database/database_util.h"
#include "webkit/common/database/database_identifier.h"

using webkit_database::DatabaseUtil;

static void TestVfsFilePath(bool expected_result,
                            const char* vfs_file_name,
                            const char* expected_origin_identifier = "",
                            const char* expected_database_name = "",
                            const char* expected_sqlite_suffix = "") {
  std::string origin_identifier;
  base::string16 database_name;
  base::string16 sqlite_suffix;
  EXPECT_EQ(expected_result,
            DatabaseUtil::CrackVfsFileName(ASCIIToUTF16(vfs_file_name),
                                           &origin_identifier,
                                           &database_name,
                                           &sqlite_suffix));
  EXPECT_EQ(expected_origin_identifier, origin_identifier);
  EXPECT_EQ(ASCIIToUTF16(expected_database_name), database_name);
  EXPECT_EQ(ASCIIToUTF16(expected_sqlite_suffix), sqlite_suffix);
}

static GURL ToAndFromOriginIdentifier(const GURL origin_url) {
  std::string id = webkit_database::GetIdentifierFromOrigin(origin_url);
  return webkit_database::GetOriginFromIdentifier(id);
}

static void TestValidOriginIdentifier(bool expected_result,
                                      const std::string& id) {
  EXPECT_EQ(expected_result,
            DatabaseUtil::IsValidOriginIdentifier(id));
}

namespace webkit_database {

// Test DatabaseUtil::CrackVfsFilePath on various inputs.
TEST(DatabaseUtilTest, CrackVfsFilePathTest) {
  TestVfsFilePath(true, "origin/#", "origin", "", "");
  TestVfsFilePath(true, "origin/#suffix", "origin", "", "suffix");
  TestVfsFilePath(true, "origin/db_name#", "origin", "db_name", "");
  TestVfsFilePath(true, "origin/db_name#suffix", "origin", "db_name", "suffix");
  TestVfsFilePath(false, "origindb_name#");
  TestVfsFilePath(false, "origindb_name#suffix");
  TestVfsFilePath(false, "origin/db_name");
  TestVfsFilePath(false, "origin#db_name/suffix");
  TestVfsFilePath(false, "/db_name#");
  TestVfsFilePath(false, "/db_name#suffix");
}

TEST(DatabaseUtilTest, OriginIdentifiers) {
  const GURL kFileOrigin(GURL("file:///").GetOrigin());
  const GURL kHttpOrigin(GURL("http://bar/").GetOrigin());
  EXPECT_EQ(kFileOrigin, ToAndFromOriginIdentifier(kFileOrigin));
  EXPECT_EQ(kHttpOrigin, ToAndFromOriginIdentifier(kHttpOrigin));
}

TEST(DatabaseUtilTest, IsValidOriginIdentifier) {
  TestValidOriginIdentifier(true,  "http_bar_0");
  TestValidOriginIdentifier(true,  "");
  TestValidOriginIdentifier(false, "bad..id");
  TestValidOriginIdentifier(false, "bad/id");
  TestValidOriginIdentifier(false, "bad\\id");
  TestValidOriginIdentifier(false, std::string("bad\0id", 6));
}

}  // namespace webkit_database
