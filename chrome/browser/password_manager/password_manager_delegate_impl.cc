// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/password_manager/password_manager_delegate_impl.h"

#include "base/command_line.h"
#include "base/memory/singleton.h"
#include "base/metrics/histogram.h"
#include "base/strings/utf_string_conversions.h"
#include "base/timer/elapsed_timer.h"
#include "chrome/browser/infobars/confirm_infobar_delegate.h"
#include "chrome/browser/infobars/infobar.h"
#include "chrome/browser/infobars/infobar_service.h"
#include "chrome/browser/password_manager/password_form_manager.h"
#include "chrome/browser/password_manager/password_manager.h"
#include "chrome/browser/password_manager/password_manager_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/passwords/manage_passwords_bubble_ui_controller.h"
#include "chrome/browser/ui/sync/one_click_signin_helper.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/chrome_version_info.h"
#include "components/password_manager/core/browser/password_manager_metrics_util.h"
#include "content/public/browser/navigation_details.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "google_apis/gaia/gaia_urls.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "grit/theme_resources.h"
#include "ui/base/l10n/l10n_util.h"

namespace {

void ReportOsPassword() {
  password_manager_util::OsPasswordStatus status =
      password_manager_util::GetOsPasswordStatus();

  UMA_HISTOGRAM_ENUMERATION("PasswordManager.OsPasswordStatus",
                            status,
                            password_manager_util::MAX_PASSWORD_STATUS);
}

}  // namespace

// SavePasswordInfoBarDelegate ------------------------------------------------

// After a successful *new* login attempt, we take the PasswordFormManager in
// provisional_save_manager_ and move it to a SavePasswordInfoBarDelegate while
// the user makes up their mind with the "save password" infobar. Note if the
// login is one we already know about, the end of the line is
// provisional_save_manager_ because we just update it on success and so such
// forms never end up in an infobar.
class SavePasswordInfoBarDelegate : public ConfirmInfoBarDelegate {
 public:
  // If we won't be showing the one-click signin infobar, creates a save
  // password infobar and delegate and adds the infobar to the InfoBarService
  // for |web_contents|.  |uma_histogram_suffix| is empty, or one of the
  // "group_X" suffixes used in the histogram names for infobar usage reporting;
  // if empty, the usage is not reported, otherwise the suffix is used to choose
  // the right histogram.
  static void Create(content::WebContents* web_contents,
                     PasswordFormManager* form_to_save,
                     const std::string& uma_histogram_suffix);

 private:
  enum ResponseType {
    NO_RESPONSE = 0,
    REMEMBER_PASSWORD,
    NEVER_REMEMBER_PASSWORD,
    INFOBAR_DISMISSED,
    NUM_RESPONSE_TYPES,
  };

  SavePasswordInfoBarDelegate(PasswordFormManager* form_to_save,
                              const std::string& uma_histogram_suffix);
  virtual ~SavePasswordInfoBarDelegate();

  // InfoBarDelegate
  virtual bool ShouldExpire(const content::LoadCommittedDetails& details)
      const OVERRIDE;

  // ConfirmInfoBarDelegate
  virtual int GetIconID() const OVERRIDE;
  virtual Type GetInfoBarType() const OVERRIDE;
  virtual base::string16 GetMessageText() const OVERRIDE;
  virtual base::string16 GetButtonLabel(InfoBarButton button) const OVERRIDE;
  virtual bool Accept() OVERRIDE;
  virtual bool Cancel() OVERRIDE;
  virtual void InfoBarDismissed() OVERRIDE;

  virtual InfoBarAutomationType GetInfoBarAutomationType() const OVERRIDE;

  // The PasswordFormManager managing the form we're asking the user about,
  // and should update as per her decision.
  scoped_ptr<PasswordFormManager> form_to_save_;

  // Used to track the results we get from the info bar.
  ResponseType infobar_response_;

  // Measures the "Save password?" prompt lifetime. Used to report an UMA
  // signal.
  base::ElapsedTimer timer_;

  // The group name corresponding to the domain name of |form_to_save_| if the
  // form is on a monitored domain. Otherwise, an empty string.
  const std::string uma_histogram_suffix_;

  DISALLOW_COPY_AND_ASSIGN(SavePasswordInfoBarDelegate);
};

// static
void SavePasswordInfoBarDelegate::Create(
    content::WebContents* web_contents,
    PasswordFormManager* form_to_save,
    const std::string& uma_histogram_suffix) {
#if defined(ENABLE_ONE_CLICK_SIGNIN)
  // Don't show the password manager infobar if this form is for a google
  // account and we are going to show the one-click signin infobar.
  GURL realm(form_to_save->realm());
  // TODO(mathp): Checking only against associated_username() causes a bug
  // referenced here: crbug.com/133275
  if (((realm == GaiaUrls::GetInstance()->gaia_login_form_realm()) ||
       (realm == GURL("https://www.google.com/"))) &&
      OneClickSigninHelper::CanOffer(
          web_contents, OneClickSigninHelper::CAN_OFFER_FOR_INTERSTITAL_ONLY,
          base::UTF16ToUTF8(form_to_save->associated_username()), NULL))
    return;
#endif

  InfoBarService::FromWebContents(web_contents)->AddInfoBar(
      ConfirmInfoBarDelegate::CreateInfoBar(scoped_ptr<ConfirmInfoBarDelegate>(
          new SavePasswordInfoBarDelegate(form_to_save,
                                          uma_histogram_suffix))));
}

SavePasswordInfoBarDelegate::SavePasswordInfoBarDelegate(
    PasswordFormManager* form_to_save,
    const std::string& uma_histogram_suffix)
    : ConfirmInfoBarDelegate(),
      form_to_save_(form_to_save),
      infobar_response_(NO_RESPONSE),
      uma_histogram_suffix_(uma_histogram_suffix) {
  if (!uma_histogram_suffix_.empty()) {
    password_manager_metrics_util::LogUMAHistogramBoolean(
        "PasswordManager.SavePasswordPromptDisplayed_" + uma_histogram_suffix_,
        true);
  }
}

SavePasswordInfoBarDelegate::~SavePasswordInfoBarDelegate() {
  UMA_HISTOGRAM_ENUMERATION("PasswordManager.InfoBarResponse",
                            infobar_response_, NUM_RESPONSE_TYPES);

  // The shortest period for which the prompt needs to live, so that we don't
  // consider it killed prematurely, as might happen, e.g., if a pre-rendered
  // page gets swapped in (and the current WebContents is destroyed).
  const base::TimeDelta kMinimumPromptDisplayTime =
      base::TimeDelta::FromSeconds(1);

  if (!uma_histogram_suffix_.empty()) {
    password_manager_metrics_util::LogUMAHistogramEnumeration(
        "PasswordManager.SavePasswordPromptResponse_" + uma_histogram_suffix_,
        infobar_response_, NUM_RESPONSE_TYPES);
    password_manager_metrics_util::LogUMAHistogramBoolean(
        "PasswordManager.SavePasswordPromptDisappearedQuickly_" +
            uma_histogram_suffix_,
        timer_.Elapsed() < kMinimumPromptDisplayTime);
  }
}

bool SavePasswordInfoBarDelegate::ShouldExpire(
    const content::LoadCommittedDetails& details) const {
  bool is_not_redirect = !(details.entry->GetTransitionType() &
                           content::PAGE_TRANSITION_IS_REDIRECT_MASK);
  return is_not_redirect && InfoBarDelegate::ShouldExpire(details);
}

int SavePasswordInfoBarDelegate::GetIconID() const {
  return IDR_INFOBAR_SAVE_PASSWORD;
}

InfoBarDelegate::Type SavePasswordInfoBarDelegate::GetInfoBarType() const {
  return PAGE_ACTION_TYPE;
}

base::string16 SavePasswordInfoBarDelegate::GetMessageText() const {
  return l10n_util::GetStringUTF16(IDS_PASSWORD_MANAGER_SAVE_PASSWORD_PROMPT);
}

base::string16 SavePasswordInfoBarDelegate::GetButtonLabel(
    InfoBarButton button) const {
  return l10n_util::GetStringUTF16((button == BUTTON_OK) ?
      IDS_PASSWORD_MANAGER_SAVE_BUTTON : IDS_PASSWORD_MANAGER_BLACKLIST_BUTTON);
}

bool SavePasswordInfoBarDelegate::Accept() {
  DCHECK(form_to_save_.get());
  form_to_save_->Save();
  infobar_response_ = REMEMBER_PASSWORD;
  return true;
}

bool SavePasswordInfoBarDelegate::Cancel() {
  DCHECK(form_to_save_.get());
  form_to_save_->PermanentlyBlacklist();
  infobar_response_ = NEVER_REMEMBER_PASSWORD;
  return true;
}

void SavePasswordInfoBarDelegate::InfoBarDismissed() {
  DCHECK(form_to_save_.get());
  infobar_response_ = INFOBAR_DISMISSED;
}

InfoBarDelegate::InfoBarAutomationType
    SavePasswordInfoBarDelegate::GetInfoBarAutomationType() const {
  return PASSWORD_INFOBAR;
}


// PasswordManagerDelegateImpl ------------------------------------------------

DEFINE_WEB_CONTENTS_USER_DATA_KEY(PasswordManagerDelegateImpl);

PasswordManagerDelegateImpl::PasswordManagerDelegateImpl(
    content::WebContents* web_contents)
    : web_contents_(web_contents), driver_(web_contents, this) {
  // Avoid checking OS password until later on in browser startup
  // since it calls a few Windows APIs.
  base::MessageLoopProxy::current()->PostDelayedTask(
      FROM_HERE,
      base::Bind(&ReportOsPassword),
      base::TimeDelta::FromSeconds(10));
}

PasswordManagerDelegateImpl::~PasswordManagerDelegateImpl() {
}

void PasswordManagerDelegateImpl::PromptUserToSavePassword(
    PasswordFormManager* form_to_save) {
  if (CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableSavePasswordBubble)) {
    ManagePasswordsBubbleUIController* manage_passwords_bubble_ui_controller =
        ManagePasswordsBubbleUIController::FromWebContents(web_contents_);
    if (manage_passwords_bubble_ui_controller) {
      manage_passwords_bubble_ui_controller->OnPasswordSubmitted(form_to_save);
    } else {
      delete form_to_save;
    }
  } else {
    std::string uma_histogram_suffix(
        password_manager_metrics_util::GroupIdToString(
            password_manager_metrics_util::MonitoredDomainGroupId(
                form_to_save->realm(), GetProfile()->GetPrefs())));
    SavePasswordInfoBarDelegate::Create(
        web_contents_, form_to_save, uma_histogram_suffix);
  }
}

void PasswordManagerDelegateImpl::PasswordWasAutofilled(
    const autofill::PasswordFormMap& best_matches) const {
  ManagePasswordsBubbleUIController* manage_passwords_bubble_ui_controller =
      ManagePasswordsBubbleUIController::FromWebContents(web_contents_);
  if (manage_passwords_bubble_ui_controller &&
      CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableSavePasswordBubble)) {
    manage_passwords_bubble_ui_controller->OnPasswordAutofilled(best_matches);
  }
}

Profile* PasswordManagerDelegateImpl::GetProfile() {
  return Profile::FromBrowserContext(web_contents_->GetBrowserContext());
}

PrefService* PasswordManagerDelegateImpl::GetPrefs() {
  return GetProfile()->GetPrefs();
}

PasswordManagerDriver* PasswordManagerDelegateImpl::GetDriver() {
  return &driver_;
}

base::FieldTrial::Probability
PasswordManagerDelegateImpl::GetProbabilityForExperiment(
    const std::string& experiment_name) {
  base::FieldTrial::Probability enabled_probability = 0;
  if (experiment_name == PasswordManager::kOtherPossibleUsernamesExperiment) {
    switch (chrome::VersionInfo::GetChannel()) {
      case chrome::VersionInfo::CHANNEL_DEV:
      case chrome::VersionInfo::CHANNEL_BETA:
        enabled_probability = 50;
        break;
      default:
        break;
    }
  }
  return enabled_probability;
}

// static
PasswordGenerationManager*
PasswordManagerDelegateImpl::GetGenerationManagerFromWebContents(
    content::WebContents* contents) {
  PasswordManagerDelegateImpl* delegate =
      PasswordManagerDelegateImpl::FromWebContents(contents);
  if (!delegate)
    return NULL;
  return delegate->GetDriver()->GetPasswordGenerationManager();
}

// static
PasswordManager* PasswordManagerDelegateImpl::GetManagerFromWebContents(
    content::WebContents* contents) {
  PasswordManagerDelegateImpl* delegate =
      PasswordManagerDelegateImpl::FromWebContents(contents);
  if (!delegate)
    return NULL;
  return delegate->GetDriver()->GetPasswordManager();
}
