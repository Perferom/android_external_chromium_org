// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/multi_profile_user_controller.h"

#include "base/memory/scoped_ptr.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/chromeos/login/fake_user_manager.h"
#include "chrome/browser/chromeos/login/multi_profile_user_controller_delegate.h"
#include "chrome/browser/chromeos/login/user_manager.h"
#include "chrome/browser/chromeos/policy/policy_cert_service.h"
#include "chrome/browser/chromeos/policy/policy_cert_service_factory.h"
#include "chrome/browser/chromeos/policy/policy_cert_verifier.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/scoped_testing_local_state.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_pref_service_syncable.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/testing_profile_manager.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "net/cert/x509_certificate.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeos {

namespace {

const char* kUsers[] = {"a@gmail.com", "b@gmail.com" };

struct BehaviorTestCase {
  const char* primary;
  const char* secondary;
  bool expected_allowed;
};

const BehaviorTestCase kBehaviorTestCases[] = {
  {
    MultiProfileUserController::kBehaviorUnrestricted,
    MultiProfileUserController::kBehaviorUnrestricted,
    true,
  },
  {
    MultiProfileUserController::kBehaviorUnrestricted,
    MultiProfileUserController::kBehaviorPrimaryOnly,
    false,
  },
  {
    MultiProfileUserController::kBehaviorUnrestricted,
    MultiProfileUserController::kBehaviorNotAllowed,
    false,
  },
  {
    MultiProfileUserController::kBehaviorPrimaryOnly,
    MultiProfileUserController::kBehaviorUnrestricted,
    true,
  },
  {
    MultiProfileUserController::kBehaviorPrimaryOnly,
    MultiProfileUserController::kBehaviorPrimaryOnly,
    false,
  },
  {
    MultiProfileUserController::kBehaviorPrimaryOnly,
    MultiProfileUserController::kBehaviorNotAllowed,
    false,
  },
  {
    MultiProfileUserController::kBehaviorNotAllowed,
    MultiProfileUserController::kBehaviorUnrestricted,
    false,
  },
  {
    MultiProfileUserController::kBehaviorNotAllowed,
    MultiProfileUserController::kBehaviorPrimaryOnly,
    false,
  },
  {
    MultiProfileUserController::kBehaviorNotAllowed,
    MultiProfileUserController::kBehaviorNotAllowed,
    false,
  },
};

policy::PolicyCertVerifier* g_policy_cert_verifier_for_factory = NULL;

BrowserContextKeyedService* TestPolicyCertServiceFactory(
    content::BrowserContext* context) {
  return policy::PolicyCertService::CreateForTesting(
      kUsers[0], g_policy_cert_verifier_for_factory, UserManager::Get())
      .release();
}

}  // namespace

class MultiProfileUserControllerTest
    : public testing::Test,
      public MultiProfileUserControllerDelegate {
 public:
  MultiProfileUserControllerTest()
      : profile_manager_(TestingBrowserProcess::GetGlobal()),
        fake_user_manager_(new FakeUserManager),
        user_manager_enabler_(fake_user_manager_),
        user_not_allowed_count_(0) {}
  virtual ~MultiProfileUserControllerTest() {}

  virtual void SetUp() OVERRIDE {
    ASSERT_TRUE(profile_manager_.SetUp());
    controller_.reset(new MultiProfileUserController(
        this, TestingBrowserProcess::GetGlobal()->local_state()));

    for (size_t i = 0; i < arraysize(kUsers); ++i) {
      const std::string user_email(kUsers[i]);
      const User* user = fake_user_manager_->AddUser(user_email);

      // Note that user profiles are created after user login in reality.
      TestingProfile* user_profile =
          profile_manager_.CreateTestingProfile(user_email);
      user_profile->set_profile_name(user_email);
      user_profiles_.push_back(user_profile);

      fake_user_manager_->SetProfileForUser(user, user_profile);
    }
  }

  void LoginUser(size_t user_index) {
    ASSERT_LT(user_index, arraysize(kUsers));
    fake_user_manager_->LoginUser(kUsers[user_index]);
    controller_->StartObserving(user_profiles_[user_index]);
  }

  void SetOwner(size_t user_index) {
    fake_user_manager_->set_owner_email(kUsers[user_index]);
  }

  PrefService* GetUserPrefs(size_t user_index) {
    return user_profiles_[user_index]->GetPrefs();
  }

  void SetPrefBehavior(size_t user_index, const std::string& behavior) {
    GetUserPrefs(user_index)->SetString(prefs::kMultiProfileUserBehavior,
                                        behavior);
  }

  std::string GetCachedBehavior(size_t user_index) {
    return controller_->GetCachedValue(kUsers[user_index]);
  }

  void SetCachedBehavior(size_t user_index,
                         const std::string& behavior) {
    controller_->SetCachedValue(kUsers[user_index], behavior);
  }

  void ResetCounts() {
    user_not_allowed_count_ = 0;
  }

  // MultiProfileUserControllerDeleagte overrides:
  virtual void OnUserNotAllowed() OVERRIDE {
    ++user_not_allowed_count_;
  }

  MultiProfileUserController* controller() { return controller_.get(); }
  int user_not_allowed_count() const { return user_not_allowed_count_; }

  TestingProfile* profile(int index) {
    return user_profiles_[index];
  }

 private:
  content::TestBrowserThreadBundle threads_;
  TestingProfileManager profile_manager_;
  FakeUserManager* fake_user_manager_;  // Not owned
  ScopedUserManagerEnabler user_manager_enabler_;

  scoped_ptr<MultiProfileUserController> controller_;

  std::vector<TestingProfile*> user_profiles_;

  int user_not_allowed_count_;

  DISALLOW_COPY_AND_ASSIGN(MultiProfileUserControllerTest);
};

// Tests that everyone is allowed before a session starts.
TEST_F(MultiProfileUserControllerTest, AllAllowedBeforeLogin) {
  const char* kTestCases[] = {
    MultiProfileUserController::kBehaviorUnrestricted,
    MultiProfileUserController::kBehaviorPrimaryOnly,
    MultiProfileUserController::kBehaviorNotAllowed,
  };
  for (size_t i = 0; i < arraysize(kTestCases); ++i) {
    SetCachedBehavior(0, kTestCases[i]);
    EXPECT_TRUE(controller()->IsUserAllowedInSession(kUsers[0]))
        << "Case " << i;
  }
}

// Tests that invalid cache value would become the default "unrestricted".
TEST_F(MultiProfileUserControllerTest, InvalidCacheBecomesDefault) {
  const char kBad[] = "some invalid value";
  SetCachedBehavior(0, kBad);
  EXPECT_EQ(MultiProfileUserController::kBehaviorUnrestricted,
            GetCachedBehavior(0));
}

// Tests that cached behavior value changes with user pref after login.
TEST_F(MultiProfileUserControllerTest, CachedBehaviorUpdate) {
  LoginUser(0);

  const char* kTestCases[] = {
    MultiProfileUserController::kBehaviorUnrestricted,
    MultiProfileUserController::kBehaviorPrimaryOnly,
    MultiProfileUserController::kBehaviorNotAllowed,
    MultiProfileUserController::kBehaviorUnrestricted,
  };
  for (size_t i = 0; i < arraysize(kTestCases); ++i) {
    SetPrefBehavior(0, kTestCases[i]);
    EXPECT_EQ(kTestCases[i], GetCachedBehavior(0));
  }
}

// Tests that compromised cache value would be fixed and pref value is checked
// upon login.
TEST_F(MultiProfileUserControllerTest, CompromisedCacheFixedOnLogin) {
  SetPrefBehavior(0, MultiProfileUserController::kBehaviorPrimaryOnly);
  SetCachedBehavior(0, MultiProfileUserController::kBehaviorUnrestricted);
  EXPECT_EQ(MultiProfileUserController::kBehaviorUnrestricted,
            GetCachedBehavior(0));
  LoginUser(0);
  EXPECT_EQ(MultiProfileUserController::kBehaviorPrimaryOnly,
            GetCachedBehavior(0));

  EXPECT_EQ(0, user_not_allowed_count());
  SetPrefBehavior(1, MultiProfileUserController::kBehaviorPrimaryOnly);
  SetCachedBehavior(1, MultiProfileUserController::kBehaviorUnrestricted);
  EXPECT_EQ(MultiProfileUserController::kBehaviorUnrestricted,
            GetCachedBehavior(1));
  LoginUser(1);
  EXPECT_EQ(MultiProfileUserController::kBehaviorPrimaryOnly,
            GetCachedBehavior(1));
  EXPECT_EQ(1, user_not_allowed_count());
}

// Tests cases before the second user login.
TEST_F(MultiProfileUserControllerTest, IsSecondaryAllowed) {
  LoginUser(0);

  for (size_t i = 0; i < arraysize(kBehaviorTestCases); ++i) {
    SetPrefBehavior(0, kBehaviorTestCases[i].primary);
    SetCachedBehavior(1, kBehaviorTestCases[i].secondary);
    EXPECT_EQ(kBehaviorTestCases[i].expected_allowed,
              controller()->IsUserAllowedInSession(kUsers[1])) << "Case " << i;
  }
}

// Tests user behavior changes within a two-user session.
TEST_F(MultiProfileUserControllerTest, PrimaryBehaviorChange) {
  LoginUser(0);
  LoginUser(1);

  for (size_t i = 0; i < arraysize(kBehaviorTestCases); ++i) {
    SetPrefBehavior(0, MultiProfileUserController::kBehaviorUnrestricted);
    SetPrefBehavior(1, MultiProfileUserController::kBehaviorUnrestricted);
    ResetCounts();

    SetPrefBehavior(0, kBehaviorTestCases[i].primary);
    SetPrefBehavior(1, kBehaviorTestCases[i].secondary);
    EXPECT_EQ(kBehaviorTestCases[i].expected_allowed,
              user_not_allowed_count() == 0) << "Case " << i;
  }
}

// Tests that owner could not be a secondary user.
TEST_F(MultiProfileUserControllerTest, NoSecondaryOwner) {
  LoginUser(0);
  SetOwner(1);

  EXPECT_FALSE(controller()->IsUserAllowedInSession(kUsers[1]));

  EXPECT_EQ(0, user_not_allowed_count());
  LoginUser(1);
  EXPECT_EQ(1, user_not_allowed_count());
}

TEST_F(MultiProfileUserControllerTest,
       UsedPolicyCertificatesAllowedForPrimary) {
  // Verifies that any user can sign-in as the primary user, regardless of the
  // tainted state.
  policy::PolicyCertServiceFactory::SetUsedPolicyCertificates(kUsers[0]);
  EXPECT_TRUE(controller()->IsUserAllowedInSession(kUsers[0]));
  EXPECT_TRUE(controller()->IsUserAllowedInSession(kUsers[1]));
}

TEST_F(MultiProfileUserControllerTest,
       UsedPolicyCertificatesDisallowedForSecondary) {
  // Verifies that if a regular user is signed-in then other regular users can
  // be added but tainted users can't.
  LoginUser(1);

  // TODO(xiyuan): Remove the following SetPrefBehavor when default is
  // changed back to enabled.
  SetPrefBehavior(1, MultiProfileUserController::kBehaviorUnrestricted);

  EXPECT_TRUE(controller()->IsUserAllowedInSession(kUsers[0]));
  policy::PolicyCertServiceFactory::SetUsedPolicyCertificates(kUsers[0]);
  EXPECT_FALSE(controller()->IsUserAllowedInSession(kUsers[0]));
}

TEST_F(MultiProfileUserControllerTest,
       UsedPolicyCertificatesDisallowsSecondaries) {
  // Verifies that if a tainted user is signed-in then no other users can
  // be added.
  policy::PolicyCertServiceFactory::SetUsedPolicyCertificates(kUsers[0]);
  LoginUser(0);

  // Double parenthesis to avoid http://en.wikipedia.org/wiki/Most_vexing_parse.
  policy::PolicyCertVerifier verifier((base::Closure()));
  g_policy_cert_verifier_for_factory = &verifier;
  ASSERT_TRUE(
      policy::PolicyCertServiceFactory::GetInstance()->SetTestingFactoryAndUse(
          profile(0), TestPolicyCertServiceFactory));

  EXPECT_FALSE(controller()->IsUserAllowedInSession(kUsers[1]));
  policy::PolicyCertServiceFactory::SetUsedPolicyCertificates(kUsers[1]);
  EXPECT_FALSE(controller()->IsUserAllowedInSession(kUsers[1]));

  // Flush tasks posted to IO.
  base::RunLoop().RunUntilIdle();
}

TEST_F(MultiProfileUserControllerTest,
       PolicyCertificatesInMemoryDisallowsSecondaries) {
  // Verifies that if a user is signed-in and has policy certificates installed
  // then no other users can be added.
  LoginUser(0);

  // TODO(xiyuan): Remove the following SetPrefBehavor when default is
  // changed back to enabled.
  SetPrefBehavior(0, MultiProfileUserController::kBehaviorUnrestricted);

  // Double parenthesis to avoid http://en.wikipedia.org/wiki/Most_vexing_parse.
  policy::PolicyCertVerifier verifier((base::Closure()));
  g_policy_cert_verifier_for_factory = &verifier;
  ASSERT_TRUE(
      policy::PolicyCertServiceFactory::GetInstance()->SetTestingFactoryAndUse(
          profile(0), TestPolicyCertServiceFactory));
  policy::PolicyCertService* service =
      policy::PolicyCertServiceFactory::GetForProfile(profile(0));
  ASSERT_TRUE(service);

  EXPECT_FALSE(service->has_policy_certificates());
  EXPECT_TRUE(controller()->IsUserAllowedInSession(kUsers[1]));

  net::CertificateList certificates;
  certificates.push_back(new net::X509Certificate(
      "subject", "issuer", base::Time(), base::Time()));
  service->OnTrustAnchorsChanged(certificates);
  EXPECT_TRUE(service->has_policy_certificates());
  EXPECT_FALSE(controller()->IsUserAllowedInSession(kUsers[1]));

  // Flush tasks posted to IO.
  base::RunLoop().RunUntilIdle();
}

}  // namespace chromeos
