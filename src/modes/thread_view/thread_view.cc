# include <iostream>
# include <fstream>
# include <atomic>
# include <vector>
# include <algorithm>
# include <chrono>
# include <mutex>
# include <condition_variable>
# include <functional>

# include <gtkmm.h>
# include <webkit2/webkit2.h>
# include <gio/gio.h>
# include <boost/property_tree/ptree.hpp>

# include "thread_view.hh"
# include "theme.hh"
# include "page_client.hh"

# include "main_window.hh"
# include "message_thread.hh"
# include "chunk.hh"
# include "crypto.hh"
# include "db.hh"
# include "utils/utils.hh"
# include "utils/address.hh"
# include "utils/vector_utils.hh"
# include "utils/ustring_utils.hh"
# include "utils/gravatar.hh"
# include "utils/cmd.hh"
# include "utils/gmime/gmime-compat.h"
# ifndef DISABLE_PLUGINS
  # include "plugin/manager.hh"
# endif
# include "actions/action.hh"
# include "actions/cmdaction.hh"
# include "actions/tag_action.hh"
# include "actions/toggle_action.hh"
# include "actions/difftag_action.hh"
# include "modes/mode.hh"
# include "modes/reply_message.hh"
# include "modes/forward_message.hh"
# include "modes/raw_message.hh"
# include "modes/thread_index/thread_index.hh"
# include "theme.hh"

using namespace std;
using boost::property_tree::ptree;

namespace Astroid {

  ThreadView::ThreadView (MainWindow * mw) : Mode (mw) { //
    page_client.thread_view = this;

    const ptree& config = astroid->config ("thread_view");
    indent_messages = config.get<bool> ("indent_messages");
    open_html_part_external = config.get<bool> ("open_html_part_external");
    open_external_link = config.get<string> ("open_external_link");

    enable_code_prettify = config.get<bool> ("code_prettify.enable");
    enable_code_prettify_for_patches = config.get<bool> ("code_prettify.enable_for_patches");

    expand_flagged = config.get<bool> ("expand_flagged");

    ustring cp_only_tags = config.get<string> ("code_prettify.for_tags");
    if (cp_only_tags.length() > 0) {
      code_prettify_only_tags = VectorUtils::split_and_trim (cp_only_tags, ",");
    }

    code_prettify_code_tag = config.get<string> ("code_prettify.code_tag");

    page_client.enable_gravatar = config.get<bool>("gravatar.enable");
    unread_delay = config.get<double>("mark_unread_delay");

    ready = false;

    pack_start (scroll, true, true, 0);

    /* WebKit: set up webkit web view */

    /* content manager for adding theme and script */
    webcontent = webkit_user_content_manager_new ();

    /* add style sheet */
    WebKitUserStyleSheet * style = webkit_user_style_sheet_new (
        theme.thread_view_css.c_str(),
        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_STYLE_LEVEL_AUTHOR,
        NULL, NULL);
    webkit_user_content_manager_add_style_sheet (webcontent, style);

    /* create webview */
    webview = WEBKIT_WEB_VIEW (webkit_web_view_new_with_user_content_manager (webcontent));

    websettings = WEBKIT_SETTINGS (webkit_settings_new_with_settings (
        "enable-javascript", TRUE,
        "enable-java", FALSE,
        "enable-plugins", FALSE,
        "auto-load-images", TRUE,
        "enable-dns-prefetching", FALSE,
        "enable-fullscreen", FALSE,
        "enable-html5-database", FALSE,
        "enable-html5-local-storage", FALSE,
        "enable-mediasource", FALSE,
        "enable-offline-web-application-cache", FALSE,
        "enable-page-cache", FALSE,
        "enable-private-browsing", TRUE,
        "enable-xss-auditor", TRUE,
        "media-playback-requires-user-gesture", TRUE,
        "enable-developer-extras", TRUE, // TODO: should only enabled conditionally
        NULL));

    webkit_web_view_set_settings (webview, websettings);

    gtk_container_add (GTK_CONTAINER (scroll.gobj()), GTK_WIDGET(webview));

# ifdef DEBUG_WEBKIT
    /* Always show the inspector */
    WebKitWebInspector *inspector = webkit_web_view_get_inspector (WEBKIT_WEB_VIEW(webview));
    webkit_web_inspector_show (WEBKIT_WEB_INSPECTOR(inspector));
# endif

    scroll.show_all ();

    wk_loaded = false;

    g_signal_connect (webview, "load-changed",
        G_CALLBACK(ThreadView_on_load_changed),
        (gpointer) this );


    add_events (Gdk::KEY_PRESS_MASK);

    /* navigation requests */
    /* g_signal_connect (webview, "permissions-request", */
    /*     G_CALLBACK(ThreadView_permission_request), */
    /*     (gpointer) this); */

    g_signal_connect (webview, "decide-policy",
        G_CALLBACK(ThreadView_decide_policy),
        (gpointer) this);

    /* load attachment icon */
    ustring icon_string = "mail-attachment-symbolic";

    Glib::RefPtr<Gtk::IconTheme> theme = Gtk::IconTheme::get_default();
    attachment_icon = theme->load_icon (
        icon_string,
        ATTACHMENT_ICON_WIDTH,
        Gtk::ICON_LOOKUP_USE_BUILTIN );

    /* load marked icon */
    marked_icon = theme->load_icon (
        "object-select-symbolic",
        ATTACHMENT_ICON_WIDTH,
        Gtk::ICON_LOOKUP_USE_BUILTIN );

    register_keys ();

    show_all_children ();

# ifndef DISABLE_PLUGINS
    /* load plugins */
    plugins = new PluginManager::ThreadViewExtension (this);
# endif

  }

  ThreadView::~ThreadView () { //
    LOG (debug) << "tv: deconstruct.";
  }

  void ThreadView::pre_close () {
# ifndef DISABLE_PLUGINS
    plugins->deactivate ();
    delete plugins;
# endif
  }

  /* navigation requests  */
  void ThreadView::reload_images () {
    /* TODO: [JS]: Reload all images */
  }

  extern "C" gboolean ThreadView_permission_request (
      WebKitWebView * w,
      WebKitPermissionRequest * request,
      gpointer user_data) {

    return ((ThreadView*) user_data)->permission_request (w, request);
  }

  gboolean ThreadView::permission_request (
      WebKitWebView * /* w */,
      WebKitPermissionRequest * request) {

    /* these requests are typically full-screen or location requests */
    webkit_permission_request_allow (request);

    return true;
  }

  extern "C" gboolean ThreadView_decide_policy (
      WebKitWebView * w,
      WebKitPolicyDecision * decision,
      WebKitPolicyDecisionType decision_type,
      gpointer user_data) {

    return ((ThreadView *) user_data)->decide_policy (w, decision, decision_type);
  }

  gboolean ThreadView::decide_policy (
      WebKitWebView * /* w */,
      WebKitPolicyDecision *   decision,
      WebKitPolicyDecisionType decision_type)
  {

    switch (decision_type) {
      case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION: // navigate to {{{
        {
          WebKitNavigationPolicyDecision * navigation_decision = WEBKIT_NAVIGATION_POLICY_DECISION (decision);
          WebKitNavigationAction * nav_action = webkit_navigation_policy_decision_get_navigation_action (navigation_decision);


          // TODO: [W2]: should this request be used or ignored? Currently ignoring if we
          //             handle ourselves.

          if (webkit_navigation_action_get_navigation_type (nav_action)
              == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {

            webkit_policy_decision_ignore (decision);

            const gchar * uri_c = webkit_uri_request_get_uri (
                webkit_navigation_action_get_request (nav_action));


            ustring uri (uri_c);
            LOG (info) << "tv: navigating to: " << uri;

            ustring scheme = Glib::uri_parse_scheme (uri);

            if (scheme == "mailto") {

              uri = uri.substr (scheme.length ()+1, uri.length () - scheme.length()-1);
              UstringUtils::trim(uri);

              main_window->add_mode (new EditMessage (main_window, uri));

            } else if (scheme == "id" || scheme == "mid" ) {
              main_window->add_mode (new ThreadIndex (main_window, uri));

            } else if (scheme == "http" || scheme == "https" || scheme == "ftp") {
              open_link (uri);

            } else {

              LOG (error) << "tv: unknown uri scheme. not opening.";
            }
          }
        } // }}}
        break;

      case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION:
        webkit_policy_decision_ignore (decision);
        break;

      case WEBKIT_POLICY_DECISION_TYPE_RESPONSE: // {{{
        {
          /* request to load any resources or similar */
          WebKitResponsePolicyDecision * response = WEBKIT_RESPONSE_POLICY_DECISION (decision);
          WebKitURIRequest * request = webkit_response_policy_decision_get_request (response);

          const gchar * uri_c = webkit_uri_request_get_uri (request);
          ustring uri (uri_c);

          // prefix of local uris for loading image thumbnails
          vector<ustring> allowed_uris =
            {
              home_uri,
              "data:image/png;base64",
              "data:image/jpeg;base64",
            };

          if (page_client.enable_gravatar) {
            allowed_uris.push_back ("https://www.gravatar.com/avatar/");
          }

          if (enable_code_prettify) {
            allowed_uris.push_back (code_prettify_uri.substr (0, code_prettify_uri.rfind ("/")));
          }

# ifndef DISABLE_PLUGINS
          /* get plugin allowed uris */
          std::vector<ustring> puris = plugins->get_allowed_uris ();
          if (puris.size() > 0) {
            LOG (debug) << "tv: plugin allowed uris: " << VectorUtils::concat_tags (puris);
            allowed_uris.insert (allowed_uris.end (), puris.begin (), puris.end ());
          }
# endif

          // TODO: show cid type images and inline-attachments

          /* is this request allowed */
          if (find_if (allowed_uris.begin (), allowed_uris.end (),
                [&](ustring &a) {
                  return (uri.substr (0, a.length ()) == a);
                }) != allowed_uris.end ())
          {

            /* LOG (debug) << "tv: request: allowed: " << uri; */
            webkit_policy_decision_use (decision);

          } else {
            if (show_remote_images) {
              // TODO: use an approved-url (like geary) to only allow imgs, not JS
              //       or other content.
              LOG (warn) << "tv: remote images allowed, approving _all_ requests: " << uri;
              webkit_policy_decision_use (decision);
            } else {
              LOG (debug)<< "tv: request: denied: " << uri;
              webkit_policy_decision_ignore (decision);
              /* webkit_network_request_set_uri (request, "about:blank"); // no */
            }
          }
        } // }}}
        break;

      default:
        /* webkit_policy_decision_ignore (decision); */
        /* return true; // stop event */
        // TODO: [W2] when do we ignore and when do we use?
        return false;
    }

    return false; // stop event
  }

  void ThreadView::open_link (ustring uri) {
    LOG (debug) << "tv: opening: " << uri;

    Glib::Threads::Thread::create (
        sigc::bind (
          sigc::mem_fun (this, &ThreadView::do_open_link), uri));
  }

  void ThreadView::do_open_link (ustring uri) {
    vector<string> args = { open_external_link.c_str(), uri.c_str () };
    LOG (debug) << "tv: spawning: " << args[0] << ", " << args[1];
    string stdout;
    string stderr;
    int    exitcode;
    try {
      Glib::spawn_sync ("",
                        args,
                        Glib::SPAWN_DEFAULT | Glib::SPAWN_SEARCH_PATH,
                        sigc::slot <void> (),
                        &stdout,
                        &stderr,
                        &exitcode
                        );

    } catch (Glib::SpawnError &ex) {
      LOG (error) << "tv: exception while opening uri: " <<  ex.what ();
    }

    ustring ustdout = ustring(stdout);
    for (ustring &l : VectorUtils::split_and_trim (ustdout, ustring("\n"))) {

      LOG (debug) << l;
    }

    ustring ustderr = ustring(stderr);
    for (ustring &l : VectorUtils::split_and_trim (ustderr, ustring("\n"))) {

      LOG (debug) << l;
    }

    if (exitcode != 0) {
      LOG (error) << "tv: open link exited with code: " << exitcode;
    }
  }

  /* end navigation requests  */

  /* message loading  */
  /*
   * By the C++ standard this callback-setup is not necessarily safe, but it seems
   * to be for both g++ and clang++.
   *
   * http://stackoverflow.com/questions/2068022/in-c-is-it-safe-portable-to-use-static-member-function-pointer-for-c-api-call
   *
   * http://gtk.10911.n7.nabble.com/Using-g-signal-connect-in-class-td57137.html
   *
   * To be portable we have to use a free function declared extern "C". A
   * static member function is likely to work at least on gcc/g++, but not
   * necessarily elsewhere.
   *
   */

  extern "C" bool ThreadView_on_load_changed (
      WebKitWebView * w,
      WebKitLoadEvent load_event,
      gpointer user_data)
  {
    return ((ThreadView *) user_data)->on_load_changed (w, load_event);
  }

  bool ThreadView::on_load_changed (
      WebKitWebView * /* w */,
      WebKitLoadEvent load_event)
  {
    LOG (debug) << "tv: on_load_changed: " << load_event;
    switch (load_event) {
      case WEBKIT_LOAD_FINISHED:
        LOG (debug) << "tv: load finished.";
        {
          /* load code_prettify if enabled */
          if (enable_code_prettify) {
            bool only_tags_ok = false;
            if (code_prettify_only_tags.size () > 0) {
              if (mthread->in_notmuch) {
                for (auto &t : code_prettify_only_tags) {
                  if (mthread->has_tag (t)) {
                    only_tags_ok = true;
                    break;
                  }
                }
              } else {
                /* enable for messages not in db */
                only_tags_ok = true;
              }
            } else {
              only_tags_ok = true;
            }

            if (only_tags_ok) {
              code_is_on = true;

              // TODO: Load code prettify

              /* webkit_dom_element_set_attribute (me, "src", code_prettify_uri.c_str(), */
              /*     (err = NULL, &err)); */
            }
          }

          /* render */
          wk_loaded = true;
          render_messages ();

        }
      default:
        break;
    }

    return true;
  }

  void ThreadView::load_thread (refptr<NotmuchThread> _thread) {
    LOG (info) << "tv: load thread: " << _thread->thread_id;
    thread = _thread;

    set_label (thread->thread_id);

    Db db (Db::DbMode::DATABASE_READ_ONLY);

    auto _mthread = refptr<MessageThread>(new MessageThread (thread));
    _mthread->load_messages (&db);

    if (unread_setup) unread_checker.disconnect ();
    unread_setup = false; // reset

    load_message_thread (_mthread);
  }

  void ThreadView::load_message_thread (refptr<MessageThread> _mthread) {
    ready = false;
    mthread.clear ();
    mthread = _mthread;

    ustring s = mthread->get_subject();

    set_label (s);

    render ();
  }

  void ThreadView::on_message_changed (
      Db * /* db */,
      Message * m,
      Message::MessageChangedEvent me)
  {
    if (!edit_mode && ready) { // edit mode doesn't show tags
      if (me == Message::MessageChangedEvent::MESSAGE_TAGS_CHANGED) {
        if (m->in_notmuch && m->tid == thread->thread_id) {
          LOG (debug) << "tv: got message updated.";
          // Note that the message has already been refreshed internally

          refptr<Message> _m = refptr<Message> (m);
          _m->reference (); // since m is owned by caller

          update_message (_m);
        }
      }
    }
  }
  /* end message loading  */

  /* rendering  */

  /* general message adding and rendering  */
  void ThreadView::render () {
    LOG (info) << "render: loading html..";
    wk_loaded = false;

    /* home uri used for thread view - request will be relative this
     * non-existant (hopefully) directory. */
    home_uri = ustring::compose ("%1/%2",
        astroid->standard_paths ().config_dir.c_str(),
        UstringUtils::random_alphanumeric (120));

    ready = false;
    webkit_web_view_load_html (webview, theme.thread_view_html.c_str (), home_uri.c_str());
  }

  void ThreadView::render_messages () {
    LOG (debug) << "render: html loaded, building messages..";
    if (!wk_loaded) {
      LOG (error) << "tv: web kit not loaded.";
      return;
    }

    /* set message state vector */
    state.clear ();
    focused_message.clear ();

    for_each (mthread->messages.begin(),
              mthread->messages.end(),
              [&](refptr<Message> m) {
                add_message (m);
              });

    update_all_indent_states ();

    if (!focused_message) {
      LOG (debug) << "tv: no message focused, showing newest message.";

      focused_message = *max_element (
          mthread->messages.begin (),
          mthread->messages.end (),
          [](refptr<Message> &a, refptr<Message> &b)
            {
              return ( a->time < b->time );
            });

      expand (focused_message);

      focus_message (focused_message);
    }

    emit_ready ();

    if (!unread_setup) {
      /* there's potentially a small chance that scroll_to_message gets an
       * on_scroll_vadjustment_change emitted before we get here. probably not, since
       * it is the same thread - but still.. */
      unread_setup = true;

      if (unread_delay > 0) {
        Glib::signal_timeout ().connect (
            sigc::mem_fun (this, &ThreadView::unread_check), std::max (80., (unread_delay * 1000.) / 2));
      } else {
        unread_check ();
      }
    }
  }

  void ThreadView::update_all_indent_states () {
    for (auto &m : mthread->messages) {
      update_indent_state (m);
    }
  }

  // TODO: [JS] [REIMPLEMENT]
  void ThreadView::update_indent_state (refptr<Message> m) {
  }

  void ThreadView::add_message (refptr<Message> m) {
    LOG (debug) << "tv: adding message: " << m->mid;

    state.insert (std::pair<refptr<Message>, MessageState> (m, MessageState ()));

    if (!edit_mode) {
      m->signal_message_changed ().connect (
          sigc::mem_fun (this, &ThreadView::on_message_changed));
    }

    page_client.add_message (m);

    if (!edit_mode) {
      /* optionally hide / collapse the message */
      if (!(m->has_tag("unread") || (expand_flagged && m->has_tag("flagged")))) {

        page_client.set_hidden_state (m, true);
      } else {
        // TODO:
        /* if (!candidate_startup) */
        /*   candidate_startup = m; */
      }

      /* focus first unread message */
      if (!focused_message) {
        if (m->has_tag ("unread")) {
          focused_message = m;
        }
      }
    } else {
      focused_message = m;
    }

    page_client.focus (focused_message);

    {
      if (!edit_mode &&
           any_of (Db::draft_tags.begin (),
                   Db::draft_tags.end (),
                   [&](ustring t) {
                     return m->has_tag (t);
                   }))
      {

        /* set warning */
        set_warning (m, "This message is a draft, edit it with E or delete with D.");

      }
    }

  }

  void ThreadView::update_message (refptr<Message> m) {
    LOG (debug) << "tv: updating message: " << m->mid;

    // reset element list
    state[m].elements.clear ();
    ptree mjs = build_message (m); // this populates the MessageState with elements.
    state[m].current_element = std::max((unsigned int) (state[m].elements.size () - 1), state[m].current_element);

    /* std::stringstream js; */
    /* js << "Astroid.add_message("; */
    /* write_json (js, mjs); */
    /* js << ");"; */

    /* run_javascript (js.str ()); */

    // if the updated message was focused, the currently focused element may
    // have changed.
    if (m == focused_message)
      focus_element (focused_message, state[focused_message].current_element);
  }

  void ThreadView::remove_message (refptr<Message> m) {
    LOG (debug) << "tv: remove message: " << m->mid;
    state.erase (m);

    // TODO: escape message id EVERYWHERE!!
    // JS: Return new focused element.
    /* std::string js = "Astroid.remove_message (" + m->safe_mid () + ");"; */
    /* webkit_web_view_run_javascript (webview, js.c_str (), NULL, NULL, NULL); */

  }

  ptree ThreadView::build_message (refptr<Message> m) { // {{{
    ptree mjs;
    mjs.put ("id", m->safe_mid ());

    ptree from_node; // list of objects
    {
      Address sender (m->sender);
      ptree contact_node; // Contact object
      contact_node.put ("address", sender.email ());
      contact_node.put ("name", sender.fail_safe_name ());
      Utils::extend_ptree (from_node, contact_node);
    }
    mjs.add_child("from", from_node);

    ptree to_node; // list of objects
    for (Address &recipient: AddressList(m->to()).addresses) {
      ptree contact_node; // Contact object
      contact_node.put ("address", recipient.email ());
      contact_node.put ("name", recipient.fail_safe_name ());
      Utils::extend_ptree (to_node, contact_node);
    }
    mjs.add_child("to", to_node);

    ptree cc_node; // list of objects
    for (Address &recipient: AddressList(m->cc()).addresses) {
      ptree contact_node; // Contact object
      contact_node.put ("address", recipient.email ());
      contact_node.put ("name", recipient.fail_safe_name ());
      Utils::extend_ptree (cc_node, contact_node);
    }
    mjs.add_child("cc", cc_node);

    ptree bcc_node; // list of objects
    for (Address &recipient: AddressList(m->bcc()).addresses) {
      ptree contact_node; // Contact object
      contact_node.put ("address", recipient.email ());
      contact_node.put ("name", recipient.fail_safe_name ());
      Utils::extend_ptree (bcc_node, contact_node);
    }
    mjs.add_child("bcc", bcc_node);

    mjs.put("date.pretty", m->pretty_date ());
    mjs.put("date.verbose",  m->pretty_verbose_date (true));
    mjs.put("date.timestamp", "");

    mjs.put("subject", m->subject);

    ptree tags_node;
    if (m->in_notmuch) {
      for (ustring &tag : m->tags) {
        ptree tag_node;
        tag_node.put ("tag", tag);

        unsigned char cv[] = { 0xff, 0xff, 0xff }; // assuming tag-background is white
        auto clrs = Utils::get_tag_color (tag, cv);
        tag_node.put ("fg", clrs.first);
        tag_node.put ("bg", clrs.second);

        Utils::extend_ptree (tags_node, tag_node);
      }
    }
    mjs.add_child ("tags", tags_node);


    /* avatar */
    {
      ustring uri = "";
      auto se = Address(m->sender);
# ifdef DISABLE_PLUGINS
      if (false) {
# else
      if (plugins->get_avatar_uri (se.email (), Gravatar::DefaultStr[Gravatar::Default::RETRO], 48, m, uri)) {
# endif
        ; // all fine, use plugins avatar
      } else {
        if (page_client.enable_gravatar) {
          uri = Gravatar::get_image_uri (se.email (),Gravatar::Default::RETRO , 48);
        }
      }

      mjs.put("gravatar", uri);
    }

    mjs.put ("focused", false);
    mjs.put ("missing_content", m->missing_content);
    mjs.put ("patch", m->is_patch ());
    mjs.put ("level", m->level);
    mjs.put ("in-reply-to", m->inreplyto);

    /* preview */
    ustring bp = m->viewable_text (false, false);
    /* if (static_cast<int>(bp.size()) > MAX_PREVIEW_LEN) */
    /*   bp = bp.substr(0, MAX_PREVIEW_LEN - 3) + "..."; */

    while (true) {
      size_t i = bp.find ("<br>");

      if (i == ustring::npos) break;

      bp.erase (i, 4);
    }

    bp = Glib::Markup::escape_text (bp);
    mjs.put("preview", bp);

    /* building body */
    ptree body = build_mime_tree (m, m->root, true);
    mjs.add_child ("body", body);

    /* add mime messages */
    ptree mime_messages;
    for (refptr<Chunk> &c : m->mime_messages ()) {
      ptree message;

      message.put ("filename", c->get_filename ());
      message.put ("size", Utils::format_size(c->get_file_size ()));

      /* add signature and encryption status */
      message.put ("signed", c->issigned);
      if (c->issigned)
        message.add_child ("signature", get_signature_state (c));

      message.put ("encrypted", c->isencrypted);
      if (c->isencrypted)
        message.add_child ("encryption", get_encryption_state (c));

      MessageState::Element e (MessageState::ElementType::MimeMessage, c->id);
      state[m].elements.push_back (e);

      message.put ("eid", e.id);

      LOG (debug) << "tv: added mime message: " << e.id;
      Utils::extend_ptree (mime_messages, message);
    }
    mjs.add_child ("mime_messages", mime_messages);

    /* add attachments */
    ptree attachments;
    for (refptr<Chunk> &c : m->attachments ()) {
      ptree attachment;
      attachment.put ("filename", c->get_filename ());
      attachment.put ("size", Utils::format_size (c->get_file_size ()));
      attachment.put ("thumbnail", get_attachment_thumbnail (c));

      refptr<Glib::ByteArray> attachment_data = c->contents ();

      /* add signature and encryption status */
      attachment.put ("signed", c->issigned);
      if (c->issigned)
        attachment.add_child ("signature", get_signature_state (c));

      attachment.put ("encrypted", c->isencrypted);
      if (c->isencrypted)
        attachment.add_child ("encryption", get_encryption_state (c));

      MessageState::Element e (MessageState::ElementType::Attachment, c->id);
      state[m].elements.push_back (e);

      attachment.put ("eid", e.id);

      LOG (debug) << "tv: added attachment: " << e.id;
      Utils::extend_ptree (attachments, attachment);
    }
    mjs.add_child ("attachments", attachments);

    return mjs;
  } // }}}

  ptree ThreadView::build_mime_tree (refptr<Message> m, refptr<Chunk> c, bool root) { // {{{

    ustring mime_type;
    if (c->content_type) {
      mime_type = ustring(g_mime_content_type_get_mime_type (c->content_type));
    } else {
      mime_type = "application/octet-stream";
    }

    LOG (debug) << "create message part: " << c->id << " (siblings: " << c->siblings.size() << ") (kids: " << c->kids.size() << ")" <<
      " (attachment: " << c->attachment << ")" << " (viewable: " << c->viewable << ")" << " (mimetype: " << mime_type << ")";

    ptree part;
    part.put ("mime_type", "text/plain");
    part.put ("preferred", true);
    part.put ("encrypted", false);
    part.put ("signed", false);
    part.put ("content", "");
    part.put ("sibling", !c->siblings.empty ());
    part.add_child ("children", ptree());

    /*
     * this should not happen on the root part, but a broken message may be constructed
     * in such a way.
     */
    if (root && c->attachment) {
      /* return empty root part */
      return part;
    } else if (c->attachment) {
      return ptree ();
    }


    if (c->viewable) {
      part.put ("mime_type", mime_type);
      part.put ("preferred", c->preferred);

      part.put ("signed", c->issigned);
      if (c->issigned)
        part.add_child ("signature", get_signature_state (c));

      part.put ("encrypted", c->isencrypted);
      if (c->isencrypted)
        part.add_child ("encryption", get_encryption_state (c));

      /* TODO: filter_code_tags */
      part.put ("content", c->viewable_text (mime_type == "text/plain", true));

      /* make element */
      MessageState::Element e (MessageState::ElementType::Part, c->id);
      state[m].elements.push_back (e);

      part.put ("eid", e.id);

      LOG (debug) << "tv: added part: " << e.id;
    }

    /* recurse into children after first part so that we get the correct order
     * on elements */

    ptree children;

    for (auto &k : c->kids) {
      ptree child = build_mime_tree (m, k, false);
      if (!child.empty ()) {
        Utils::extend_ptree (children, child);
      }
    }

    if (c->viewable) {

      part.put_child ("children", children);

    } else {
      /*
       * we flatten the structure, replacing empty wrappers with an array of
       * their children.
       */

      part = children;
    }

    return part;
  } // }}}

  ptree ThreadView::get_encryption_state (refptr<Chunk> c) { // {{{
    refptr<Crypto> cr = c->crypt;

    ptree encryption;
    encryption.put ("decrypted", cr->decrypted);

    ptree recipients;

    if (cr->decrypted) {
      GMimeCertificateList * rlist = cr->rlist;
      for (int i = 0; i < g_mime_certificate_list_length (rlist); i++) {

        GMimeCertificate * ce = g_mime_certificate_list_get_certificate (rlist, i);

        const char * c = NULL;
        ustring fp = (c = g_mime_certificate_get_fingerprint (ce), c ? c : "");
        ustring nm = (c = g_mime_certificate_get_name (ce), c ? c : "");
        ustring em = (c = g_mime_certificate_get_email (ce), c ? c : "");
        ustring ky = (c = g_mime_certificate_get_key_id (ce), c ? c : "");

        ptree _r;
        _r.put ("name", nm);
        _r.put ("email", em);
        _r.put ("key", ky);

        Utils::extend_ptree (recipients, _r);
      }
    }

    encryption.put_child ("recipients", recipients);

    return encryption;
  } // }}}

  ptree ThreadView::get_signature_state (refptr<Chunk> c) { // {{{
    ptree signature;

    refptr<Crypto> cr = c->crypt;

    signature.put ("verified", cr->verified);

    ptree signatures;

    for (int i = 0; i < g_mime_signature_list_length (cr->slist); i++) {
      GMimeSignature * s = g_mime_signature_list_get_signature (cr->slist, i);
      GMimeCertificate * ce = NULL;
      if (s) ce = g_mime_signature_get_certificate (s);

      ptree _sign;
      _sign.add ("name", "");
      _sign.add ("email", "");
      _sign.add ("key", "");
      _sign.add ("status", "");
      _sign.add ("trust", "");

      ptree sig_errors;

      ustring nm, em, ky;
      ustring gd = "";

      if (ce) {
        const char * c = NULL;
        nm = (c = g_mime_certificate_get_name (ce), c ? c : "");
        em = (c = g_mime_certificate_get_email (ce), c ? c : "");
        ky = (c = g_mime_certificate_get_key_id (ce), c ? c : "");

        _sign.put ("name", nm);
        _sign.put ("email", em);
        _sign.put ("key", ky);

# if (GMIME_MAJOR_VERSION < 3)
        switch (g_mime_signature_get_status (s)) {
          case GMIME_SIGNATURE_STATUS_GOOD:
            gd = "good";
            break;

          case GMIME_SIGNATURE_STATUS_BAD:
            gd = "bad";
            // fall through

          case GMIME_SIGNATURE_STATUS_ERROR:
            if (gd.empty ()) gd = "erroneous";

            GMimeSignatureError e = g_mime_signature_get_errors (s);
            if (e & GMIME_SIGNATURE_ERROR_EXPSIG)
              Utils::extend_ptree (sig_errors, "expired");
            if (e & GMIME_SIGNATURE_ERROR_NO_PUBKEY)
              Utils::extend_ptree (sig_errors, "no-pub-key");
            if (e & GMIME_SIGNATURE_ERROR_EXPKEYSIG)
              Utils::extend_ptree (sig_errors, "expired-key-sig");
            if (e & GMIME_SIGNATURE_ERROR_REVKEYSIG)
              Utils::extend_ptree (sig_errors, "revoked-key-sig");
            if (e & GMIME_SIGNATURE_ERROR_UNSUPP_ALGO)
              Utils::extend_ptree (sig_errors, "unsupported-algo");
            break;
# else
        GMimeSignatureStatus stat = g_mime_signature_get_status (s);
        if (g_mime_signature_status_good (stat)) {
            gd = "good";
        } else if (g_mime_signature_status_bad (stat) || g_mime_signature_status_error (stat)) {

          if (g_mime_signature_status_bad (stat)) gd = "bad";
          else gd = "erroneous";

          if (stat & GMIME_SIGNATURE_STATUS_KEY_REVOKED)
            Utils::extend_ptree (sig_errors, "revoked-key");
          if (stat & GMIME_SIGNATURE_STATUS_KEY_EXPIRED)
            Utils::extend_ptree (sig_errors, "expired-key");
          if (stat & GMIME_SIGNATURE_STATUS_SIG_EXPIRED)
            Utils::extend_ptree (sig_errors, "expired-sig");
          if (stat & GMIME_SIGNATURE_STATUS_KEY_MISSING)
            Utils::extend_ptree (sig_errors, "key-missing");
          if (stat & GMIME_SIGNATURE_STATUS_CRL_MISSING)
            Utils::extend_ptree (sig_errors, "crl-missing");
          if (stat & GMIME_SIGNATURE_STATUS_CRL_TOO_OLD)
            Utils::extend_ptree (sig_errors, "crl-too-old");
          if (stat & GMIME_SIGNATURE_STATUS_BAD_POLICY)
            Utils::extend_ptree (sig_errors, "bad-policy");
          if (stat & GMIME_SIGNATURE_STATUS_SYS_ERROR)
            Utils::extend_ptree (sig_errors, "sys-error");
          if (stat & GMIME_SIGNATURE_STATUS_TOFU_CONFLICT)
            Utils::extend_ptree (sig_errors, "tofu-conflict");
# endif
        }
      } else {
        Utils::extend_ptree (sig_errors, "bad-certificate");
      }

# if (GMIME_MAJOR_VERSION < 3)
      GMimeCertificateTrust t = g_mime_certificate_get_trust (ce);
      ustring trust = "";
      switch (t) {
        case GMIME_CERTIFICATE_TRUST_NONE: trust = "none"; break;
        case GMIME_CERTIFICATE_TRUST_NEVER: trust = "never"; break;
        case GMIME_CERTIFICATE_TRUST_UNDEFINED: trust = "undefined"; break;
        case GMIME_CERTIFICATE_TRUST_MARGINAL: trust = "marginal"; break;
        case GMIME_CERTIFICATE_TRUST_FULLY: trust = "fully"; break;
        case GMIME_CERTIFICATE_TRUST_ULTIMATE: trust = "ultimate"; break;
      }
# else
      GMimeTrust t = g_mime_certificate_get_trust (ce);
      ustring trust = "";
      switch (t) {
        case GMIME_TRUST_UNKNOWN: trust = "unknown"; break;
        case GMIME_TRUST_UNDEFINED: trust = "undefined"; break;
        case GMIME_TRUST_NEVER: trust = "never"; break;
        case GMIME_TRUST_MARGINAL: trust = "marginal"; break;
        case GMIME_TRUST_FULL: trust = "full"; break;
        case GMIME_TRUST_ULTIMATE: trust = "ultimate"; break;
      }
# endif
      _sign.put ("status", gd);
      _sign.put ("trust", trust);
      _sign.put_child ("errors", sig_errors);
      Utils::extend_ptree (signatures, _sign);
    }

    signature.put_child ("signatures", signatures);

    return signature;
  } // }}}

  ustring ThreadView::get_attachment_thumbnail (refptr<Chunk> c) { // {{{
    /* set the preview image or icon on the attachment display element */
    const char * _mtype = g_mime_content_type_get_media_type (c->content_type);
    ustring mime_type;
    if (_mtype == NULL) {
      mime_type = "application/octet-stream";
    } else {
      mime_type = ustring(g_mime_content_type_get_mime_type (c->content_type));
    }

    LOG (debug) << "tv: set attachment, mime_type: " << mime_type << ", mtype: " << _mtype;

    gchar * content;
    gsize   content_size;
    ustring image_content_type;

    if ((_mtype != NULL) && (ustring(_mtype) == "image")) {
      auto mis = Gio::MemoryInputStream::create ();

      refptr<Glib::ByteArray> data = c->contents ();
      mis->add_data (data->get_data (), data->size ());

      try {

        auto pb = Gdk::Pixbuf::create_from_stream_at_scale (mis, THUMBNAIL_WIDTH, -1, true, refptr<Gio::Cancellable>());
        pb = pb->apply_embedded_orientation ();

        pb->save_to_buffer (content, content_size, "png");
        image_content_type = "image/png";
      } catch (Gdk::PixbufError &ex) {

        LOG (error) << "tv: could not create icon from attachmed image.";
        attachment_icon->save_to_buffer (content, content_size, "png"); // default type is png
        image_content_type = "image/png";
      }
    } else {
      // TODO: guess icon from mime type. Using standard icon for now.

      attachment_icon->save_to_buffer (content, content_size, "png"); // default type is png
      image_content_type = "image/png";
    }

    return assemble_data_uri (image_content_type, content, content_size);
  } // }}}

  void ThreadView::filter_code_tags (ustring &body) { // {{{
    time_t t0 = clock ();
    ustring code_tag = code_prettify_code_tag;
    ustring start_tag = code_start_tag;
    ustring stop_tag  = code_stop_tag;

    if (code_tag.length() < 1) {
      throw runtime_error ("tv: cannot have a code tag with length 0");
    }

    /* search for matching code tags */
    ustring::size_type pos = 0;

    while (true) {
      /* find first */
      ustring::size_type first = body.find (code_tag, pos);

      if (first != ustring::npos) {
        /* find second */
        ustring::size_type second = body.find (code_tag, first + code_tag.length ());
        if (second != ustring::npos) {
          /* found matching tags */
          body.erase  (first, code_tag.length());
          body.insert (first, start_tag);

          second += start_tag.length () - code_tag.length ();

          body.erase  (second, code_tag.length ());
          body.insert (second, stop_tag);
          second += stop_tag.length () - code_tag.length ();

          pos = second;
        } else {
          /* could not find matching, done */
          break;
        }

      } else {
        /* done */
        break;
      }
    }

    LOG (debug) << "tv: code filter done, time: " << ((clock() - t0) * 1000 / CLOCKS_PER_SEC) << " ms.";

  } // }}}

  // TODO: [JS] [REIMPLEMENT]
  void ThreadView::display_part (refptr<Message> message, refptr<Chunk> c, MessageState::Element el) {

  }

  /* info and warning  */
  // TODO: [JS] [REIMPLEMENT] Possibly use GtkInfoBar
  void ThreadView::set_warning (refptr<Message> m, ustring txt)
  {
    /* ustring js = "Astroid.set_warning(" + m->safe_mid () + ", " + txt + ");"; */
    /* webkit_web_view_run_javascript (webview, js.c_str (), NULL, NULL, NULL); */
  }


  void ThreadView::hide_warning (refptr<Message> m)
  {
    /* ustring js = "Astroid.hide_warning(" + m->safe_mid () + ");"; */
    /* webkit_web_view_run_javascript (webview, js.c_str (), NULL, NULL, NULL); */
  }

  // TODO: [JS] [REIMPLEMENT] Same as warning
  void ThreadView::set_info (refptr<Message> m, ustring txt)
  {
    LOG (debug) << "tv: set info: " << txt;
    /* ustring js = "Astroid.set_info(" + m->safe_mid () + ", " + txt + ");"; */
    /* webkit_web_view_run_javascript (webview, js.c_str (), NULL, NULL, NULL); */
  }

  void ThreadView::hide_info (refptr<Message> m) {
    /* ustring js = "Astroid.hide_info(" + m->safe_mid () + ");"; */
    /* webkit_web_view_run_javascript (webview, js.c_str (), NULL, NULL, NULL); */
  }
  /* end info and warning  */


  /* marked  */
  // TODO: [JS] [REIMPLEMENT]
  void ThreadView::load_marked_icon (
      refptr<Message> /* message */,
      WebKitDOMHTMLElement * div_message)
  {
# if 0
    GError *err;

    WebKitDOMHTMLElement * marked_icon_img = DomUtils::select (
        WEBKIT_DOM_NODE (div_message),
        ".marked.icon.first");

    gchar * content;
    gsize   content_size;
    marked_icon->save_to_buffer (content, content_size, "png");
    ustring image_content_type = "image/png";

    WebKitDOMHTMLImageElement *img = WEBKIT_DOM_HTML_IMAGE_ELEMENT (marked_icon_img);

    err = NULL;
    webkit_dom_element_set_attribute (WEBKIT_DOM_ELEMENT (img), "src",
        DomUtils::assemble_data_uri (image_content_type, content, content_size).c_str(), &err);

    g_object_unref (marked_icon_img);
    marked_icon_img = DomUtils::select (
        WEBKIT_DOM_NODE (div_message),
        ".marked.icon.sec");
    img = WEBKIT_DOM_HTML_IMAGE_ELEMENT (marked_icon_img);
    err = NULL;
    webkit_dom_element_set_attribute (WEBKIT_DOM_ELEMENT (img), "src",
        DomUtils::assemble_data_uri (image_content_type, content, content_size).c_str(), &err);

    g_object_unref (marked_icon_img);
# endif
  }


  //

  /* end rendering  */

  bool ThreadView::on_key_press_event (GdkEventKey * event) {
    if (!ready.load ()) return true;
    else return Mode::on_key_press_event (event);
  }

  void ThreadView::register_keys () { // {{{
    keys.title = "Thread View";

# ifdef DEBUG_WEBKIT
    keys.register_key ("C-r", "thread_view.reload",
        "Reload everything",
        [&] (Key) {
          LOG (debug) << "tv: reloading..";
          theme.load (true);

          load_message_thread (mthread);
          return true;
        });
# endif

    keys.register_key ("j", "thread_view.down",
        "Scroll down or move focus to next element",
        [&] (Key) {
          page_client.write ();
          focus_next_element ();
          return true;
        });

    keys.register_key ("C-j", "thread_view.next_element",
        "Move focus to next element",
        [&] (Key) {
          /* move focus to next element and scroll to it if necessary */

          focus_next_element (true);
          return true;
        });

    keys.register_key ("J", { Key (GDK_KEY_Down) }, "thread_view.scroll_down",
        "Scroll down",
        [&] (Key) {
          // TODO
          return true;
        });

    keys.register_key ("C-d", { Key (true, false, (guint) GDK_KEY_Down), Key (GDK_KEY_Page_Down) },
        "thread_view.page_down",
        "Page down",
        [&] (Key) {
          // TODO
          return true;
        });

    keys.register_key ("k", "thread_view.up",
        "Scroll up or move focus to previous element",
        [&] (Key) {
          focus_previous_element ();
          return true;
        });

    keys.register_key ("C-k", "thread_view.previous_element",
        "Move focus to previous element",
        [&] (Key) {
          focus_previous_element (true);
          return true;
        });

    keys.register_key ("K", { Key (GDK_KEY_Up) },
        "thread_view.scroll_up",
        "Scroll up",
        [&] (Key) {
          // TODO
          return true;
        });

    keys.register_key ("C-u", { Key (true, false, (guint) GDK_KEY_Up), Key (GDK_KEY_Page_Up) },
        "thread_view.page_up",
        "Page up",
        [&] (Key) {
          // TODO
          return true;
        });

    keys.register_key ("1", { Key (GDK_KEY_Home) },
        "thread_view.home",
        "Scroll home",
        [&] (Key) {
          focus_message (mthread->messages[0]);
          return true;
        });

    keys.register_key ("0", { Key (GDK_KEY_End) },
        "thread_view.end",
        "Scroll to end",
        [&] (Key) {
          focus_message (mthread->messages[mthread->messages.size()-1]);
          return true;
        });

    keys.register_key (Key (GDK_KEY_Return), { Key (GDK_KEY_KP_Enter), Key (true, false, (guint) GDK_KEY_space) },
        "thread_view.activate",
        "Open/expand/activate focused element",
        [&] (Key) {
          return element_action (EEnter);
        });

    keys.register_key ("s", "thread_view.save",
        "Save attachment or message",
        [&] (Key) {
          return element_action (ESave);
        });

    keys.register_key ("d", "thread_view.delete_attachment",
        "Delete attachment (if editing)",
        [&] (Key) {
          if (edit_mode) {
            /* del attachment */
            return element_action (EDelete);
          }
          return false;
        });

    keys.register_key ("e", "thread_view.expand",
        "Toggle expand",
        [&] (Key) {
          if (edit_mode) return false;

          toggle (focused_message);

          return true;
        });

    keys.register_key ("C-e", "thread_view.toggle_expand_all",
        "Toggle expand on all messages",
        [&] (Key) {
          /* toggle hidden / shown status on all messages */
          if (edit_mode) return false;

          if (all_of (mthread->messages.begin(),
                      mthread->messages.end (),
                      [&](refptr<Message> m) {
                        return !state[m].expanded;
                      }
                )) {
            /* all are hidden */
            for (auto m : mthread->messages) {
              expand (m);
            }

          } else {
            /* some are shown */
            for (auto m : mthread->messages) {
              collapse (m);
            }
          }

          return true;
        });

    keys.register_key ("t", "thread_view.mark",
        "Mark or unmark message",
        [&] (Key) {
          if (!edit_mode) {
            state[focused_message].marked = !(state[focused_message].marked);
            page_client.set_marked_state (focused_message, state[focused_message].marked);
            return true;
          }
          return false;
        });

    keys.register_key ("T", "thread_view.toggle_mark_all",
        "Toggle mark on all messages",
        [&] (Key) {
          if (!edit_mode) {

            bool any = false;
            bool all = true;

            for (auto &s : state) {
              if (s.second.marked) {
                any = true;
              } else {
                all = false;
              }

              if (any && !all) break;
            }

            for (auto &s : state) {
              if (any && !all) {
                s.second.marked = true;
              } else {
                s.second.marked = !s.second.marked;
              }
              page_client.set_marked_state (s.first, s.second.marked);
            }


            return true;
          }
          return false;
        });

    keys.register_key ("C-i", "thread_view.show_remote_images",
        "Show remote images (warning: approves all requests to remote content!)",
        [&] (Key) {
          show_remote_images = true;
          LOG (debug) << "tv: show remote images: " << show_remote_images;
          reload_images ();
          return true;
        });

    keys.register_key ("S", "thread_view.save_all_attachments",
        "Save all attachments",
        [&] (Key) {
          if (edit_mode) return false;
          save_all_attachments ();
          return true;
        });

    keys.register_key ("n", "thread_view.next_message",
        "Focus next message",
        [&] (Key) {
          focus_next_message ();
          return true;
        });

    keys.register_key ("C-n", "thread_view.next_message_expand",
        "Focus next message (and expand if necessary)",
        [&] (Key) {
          if (state[focused_message].scroll_expanded) {
            collapse (focused_message);
            state[focused_message].scroll_expanded = false;
          }

          focus_next_message ();

          state[focused_message].scroll_expanded = !expand (focused_message);
          return true;
        });

    keys.register_key ("p", "thread_view.previous_message",
        "Focus previous message",
        [&] (Key) {
          focus_previous_message (true);
          return true;
        });

    keys.register_key ("C-p", "thread_view.previous_message_expand",
        "Focus previous message (and expand if necessary)",
        [&] (Key) {
          if (state[focused_message].scroll_expanded) {
            collapse (focused_message);
            state[focused_message].scroll_expanded = false;
          }

          focus_previous_message ();

          state[focused_message].scroll_expanded = !expand (focused_message);
          return true;
        });

    keys.register_key (Key (GDK_KEY_Tab), "thread_view.next_unread",
        "Focus the next unread message",
        [&] (Key) {
          bool foundme = false;

          for (auto &m : mthread->messages) {
            if (foundme && m->has_tag ("unread")) {
              focus_message (m);
              break;
            }

            if (m == focused_message) {
              foundme = true;
            }
          }

          return true;
        });

    keys.register_key (Key (false, false, (guint) GDK_KEY_ISO_Left_Tab),
        "thread_view.previous_unread",
        "Focus the previous unread message",
        [&] (Key) {
          bool foundme = false;

          for (auto mi = mthread->messages.rbegin ();
              mi != mthread->messages.rend (); mi++) {
            if (foundme && (*mi)->has_tag ("unread")) {
              focus_message (*mi);
              break;
            }

            if (*mi == focused_message) {
              foundme = true;
            }
          }

          return true;
        });

    keys.register_key ("c", "thread_view.compose_to_sender",
        "Compose a new message to the sender of the message (or all recipients if sender is you)",
        [&] (Key) {
          if (!edit_mode) {
            Address sender = focused_message->sender;
            Address from;
            AddressList to, cc, bcc;

            /* Send to original sender if message is not from own account,
               otherwise use all recipients as in the original */
            if (astroid->accounts->is_me (sender)) {
              from = sender;
              to   = AddressList(focused_message->to  ());
              cc   = AddressList(focused_message->cc  ());
              bcc  = AddressList(focused_message->bcc ());
            } else {
              /* Not from me, just use orginal sender */
              to += sender;

              /* find the 'first' me */
              AddressList tos = focused_message->all_to_from ();

              for (Address f : tos.addresses) {
                if (astroid->accounts->is_me (f)) {
                  from = f;
                  break;
                }
              }
            }
	    main_window->add_mode (new EditMessage (main_window, to.str (),
						    from.full_address (),
						    cc.str(), bcc.str()));
          }
          return true;
        });

    keys.register_key ("r", "thread_view.reply",
        "Reply to current message",
        [&] (Key) {
          /* reply to currently focused message */
          if (!edit_mode) {
            main_window->add_mode (new ReplyMessage (main_window, focused_message));

            return true;
          }
          return false;
        });

    keys.register_key ("G", "thread_view.reply_all",
        "Reply all to current message",
        [&] (Key) {
          /* reply to currently focused message */
          if (!edit_mode) {
            main_window->add_mode (new ReplyMessage (main_window, focused_message, ReplyMessage::ReplyMode::Rep_All));

            return true;
          }
          return false;
        });

    keys.register_key ("R", "thread_view.reply_sender",
        "Reply to sender of current message",
        [&] (Key) {
          /* reply to currently focused message */
          if (!edit_mode) {
            main_window->add_mode (new ReplyMessage (main_window, focused_message, ReplyMessage::ReplyMode::Rep_Sender));

            return true;
          }
          return false;
        });

    keys.register_key ("M", "thread_view.reply_mailinglist",
        "Reply to mailinglist of current message",
        [&] (Key) {
          /* reply to currently focused message */
          if (!edit_mode) {
            main_window->add_mode (new ReplyMessage (main_window, focused_message, ReplyMessage::ReplyMode::Rep_MailingList));

            return true;
          }
          return false;
        });

    keys.register_key ("f", "thread_view.forward",
        "Forward current message",
        [&] (Key) {
          /* forward currently focused message */
          if (!edit_mode) {
            main_window->add_mode (new ForwardMessage (main_window, focused_message, ForwardMessage::FwdDisposition::FwdDefault));

            return true;
          }
          return false;
        });

    keys.register_key (UnboundKey (), "thread_view.forward_inline",
        "Forward current message inlined",
        [&] (Key) {
          /* forward currently focused message */
          if (!edit_mode) {
            main_window->add_mode (new ForwardMessage (main_window, focused_message, ForwardMessage::FwdDisposition::FwdInline));

            return true;
          }
          return false;
        });

    keys.register_key (UnboundKey (), "thread_view.forward_attached",
        "Forward current message as attachment",
        [&] (Key) {
          /* forward currently focused message */
          if (!edit_mode) {
            main_window->add_mode (new ForwardMessage (main_window, focused_message, ForwardMessage::FwdDisposition::FwdAttach));

            return true;
          }
          return false;
        });


    keys.register_key ("C-F",
        "thread_view.flat",
        "Toggle flat or indented view of messages",
        [&] (Key) {
          indent_messages = !indent_messages;
          update_all_indent_states ();
          return true;
        });

    keys.register_key ("V", "thread_view.view_raw",
        "View raw source for current message",
        [&] (Key) {
          /* view raw source of currently focused message */
          main_window->add_mode (new RawMessage (main_window, focused_message));

          return true;
        });

    keys.register_key ("E", "thread_view.edit_draft",
        "Edit currently focused message as new or draft",
        [&] (Key) {
          /* edit currently focused message as new or draft */
          if (!edit_mode) {
            main_window->add_mode (new EditMessage (main_window, focused_message));

            return true;
          }
          return false;
        });

    keys.register_key ("D", "thread_view.delete_draft",
        "Delete currently focused draft",
        [&] (Key) {
          if (!edit_mode) {

            if (any_of (Db::draft_tags.begin (),
                        Db::draft_tags.end (),
                        [&](ustring t) {
                          return focused_message->has_tag (t);
                        }))
            {
              ask_yes_no ("Do you want to delete this draft? (any changes will be lost)",
                  [&](bool yes) {
                    if (yes) {
                      EditMessage::delete_draft (focused_message);
                      close ();
                    }
                  });

              return true;
            }
          }
          return false;
        });

    keys.register_run ("thread_view.run",
	[&] (Key, ustring cmd) {
          if (!edit_mode && focused_message) {

            cmd = ustring::compose (cmd, focused_message->tid, focused_message->mid);

            astroid->actions->doit (refptr<Action> (new CmdAction (
              Cmd ("thread_view.run", cmd), focused_message->tid, focused_message->mid)));

            }
          return true;
        });

    multi_keys.register_key ("t", "thread_view.multi.toggle",
        "Toggle marked",
        [&] (Key) {
          for (auto &ms : state) {
            refptr<Message> m = ms.first;
            MessageState    s = ms.second;
            if (s.marked) {
              state[m].marked = false;
              page_client.set_marked_state (m, state[m].marked);
            }
          }
          return true;
        });

    multi_keys.register_key ("+", "thread_view.multi.tag",
        "Tag",
        [&] (Key) {
          /* TODO: Move this into a function in a similar way as multi_key_handler
           * for threadindex */


          /* ask for tags */
          main_window->enable_command (CommandBar::CommandMode::DiffTag,
              "",
              [&](ustring tgs) {
                LOG (debug) << "tv: got difftags: " << tgs;

                vector<refptr<NotmuchItem>> messages;

                for (auto &ms : state) {
                  refptr<Message> m = ms.first;
                  MessageState    s = ms.second;
                  if (s.marked) {
                    messages.push_back (m->nmmsg);
                  }
                }

                refptr<Action> ma = refptr<DiffTagAction> (DiffTagAction::create (messages, tgs));
                if (ma) {
                  main_window->actions->doit (ma);
                }
              });

          return true;
        });

    multi_keys.register_key ("C-y", "thread_view.multi.yank_mids",
        "Yank message id's",
        [&] (Key) {
          ustring ids = "";

          for (auto &m : mthread->messages) {
            MessageState s = state[m];
            if (s.marked) {
              ids += m->mid + ", ";
            }
          }

          ids = ids.substr (0, ids.length () - 2);

          auto cp = Gtk::Clipboard::get (astroid->clipboard_target);
          cp->set_text (ids);

          LOG (info) << "tv: " << ids << " copied to clipboard.";

          return true;
        });

    multi_keys.register_key ("y", "thread_view.multi.yank",
        "Yank",
        [&] (Key) {
          ustring y = "";

          for (auto &m : mthread->messages) {
            MessageState s = state[m];
            if (s.marked) {
              y += m->viewable_text (false, true);
              y += "\n";
            }
          }

          /* remove last newline */
          y = y.substr (0, y.size () - 1);

          auto cp = Gtk::Clipboard::get (astroid->clipboard_target);
          cp->set_text (y);

          LOG (info) << "tv: yanked marked messages to clipobard.";

          return true;
        });

    multi_keys.register_key ("Y", "thread_view.multi.yank_raw",
        "Yank raw",
        [&] (Key) {
          /* tries to export the messages as an mbox file */
          ustring y = "";

          for (auto &m : mthread->messages) {
            MessageState s = state[m];
            if (s.marked) {
              auto d   = m->raw_contents ();
              auto cnv = UstringUtils::bytearray_to_ustring (d);
              if (cnv.first) {
                y += ustring::compose ("From %1  %2",
                    Address(m->sender).email(),
                    m->date_asctime ()); // asctime adds a \n

                y += UstringUtils::unixify (cnv.second);
                y += "\n";
              }
            }
          }

          auto cp = Gtk::Clipboard::get (astroid->clipboard_target);
          cp->set_text (y);

          LOG (info) << "tv: yanked raw marked messages to clipobard.";

          return true;
        });

    multi_keys.register_key ("s", "thread_view.multi.save",
        "Save marked",
        [&] (Key) {
          vector<refptr<Message>> tosave;

          for (auto &ms : state) {
            refptr<Message> m = ms.first;
            MessageState    s = ms.second;
            if (s.marked) {
              tosave.push_back (m);
            }
          }

          if (!tosave.empty()) {
            LOG (debug) << "tv: saving messages: " << tosave.size();

            Gtk::FileChooserDialog dialog ("Save messages to folder..",
                Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);

            dialog.add_button ("_Cancel", Gtk::RESPONSE_CANCEL);
            dialog.add_button ("_Select", Gtk::RESPONSE_OK);
            dialog.set_current_folder (astroid->runtime_paths ().save_dir.c_str ());

            int result = dialog.run ();

            switch (result) {
              case (Gtk::RESPONSE_OK):
                {
                  string dir = dialog.get_filename ();
                  LOG (info) << "tv: saving messages to: " << dir;

                  astroid->runtime_paths ().save_dir = bfs::path (dialog.get_current_folder ());

                  for (refptr<Message> m : tosave) {
                    m->save_to (dir);
                  }

                  break;
                }

              default:
                {
                  LOG (debug) << "tv: save: cancelled.";
                }
            }
          }

          return true;
        });

    multi_keys.register_key ("p", "thread_view.multi.print",
        "Print marked messages",
        [&] (Key) {
          vector<refptr<Message>> toprint;
          for (auto &m : mthread->messages) {
            MessageState s = state[m];
            if (s.marked) {
              toprint.push_back (m);
            }
          }

# if 0
          GError * err = NULL;
          WebKitDOMDocument * d = webkit_web_view_get_dom_document (webview);
# endif

          for (auto &m : toprint) {
          // TODO: [JS] [REIMPLEMENT]
# if 0
            ustring mid = "message_" + m->mid;
            WebKitDOMElement * e = webkit_dom_document_get_element_by_id (d, mid.c_str());
            WebKitDOMDOMTokenList * class_list =
              webkit_dom_element_get_class_list (WEBKIT_DOM_ELEMENT(e));

            webkit_dom_dom_token_list_add (class_list, "print",
                (err = NULL, &err));
# endif

            /* expand */
            state[m].print_expanded = !expand (m);

          }

          bool indented = indent_messages;
          if (indent_messages) {
            indent_messages = false;
            update_all_indent_states ();
          }

          /* open print window */
          // TODO: [W2] Fix print
# if 0
          WebKitWebFrame * frame = webkit_web_view_get_main_frame (webview);
          webkit_web_frame_print (frame);
# endif

          if (indented) {
            indent_messages = true;
            update_all_indent_states ();
          }

          for (auto &m : toprint) {

            if (state[m].print_expanded) {
              collapse (m);
              state[m].print_expanded = false;
            }

# if 0
            ustring mid = "message_" + m->mid;
            WebKitDOMElement * e = webkit_dom_document_get_element_by_id (d, mid.c_str());
            WebKitDOMDOMTokenList * class_list =
              webkit_dom_element_get_class_list (WEBKIT_DOM_ELEMENT(e));

            webkit_dom_dom_token_list_remove (class_list, "print",
                (err = NULL, &err));

            g_object_unref (class_list);
            g_object_unref (e);
# endif
          }

# if 0
          g_object_unref (d);
# endif

          return true;
        });

    keys.register_key (Key (GDK_KEY_semicolon),
          "thread_view.multi",
          "Apply action to marked messages",
          [&] (Key k) {
            if (any_of (state.begin(), state.end(),
                  [](std::pair<refptr<Message>, ThreadView::MessageState> ms)
                  { return ms.second.marked; })
                )
            {

              multi_key (multi_keys, k);
            }

            return true;
          });

    keys.register_key ("N",
        "thread_view.toggle_unread",
        "Toggle the unread tag on the message",
        [&] (Key) {
          if (!edit_mode && focused_message) {

            main_window->actions->doit (refptr<Action>(new ToggleAction (refptr<NotmuchItem>(new NotmuchMessage(focused_message)), "unread")));
            state[focused_message].unread_checked = true;

          }

          return true;
        });

    keys.register_key ("*",
        "thread_view.flag",
        "Toggle the 'flagged' tag on the message",
        [&] (Key) {
          if (!edit_mode && focused_message) {

            main_window->actions->doit (refptr<Action>(new ToggleAction (refptr<NotmuchItem>(new NotmuchMessage(focused_message)), "flagged")));

          }

          return true;
        });

    keys.register_key ("a",
        "thread_view.archive_thread",
        "Toggle 'inbox' tag on the whole thread",
        [&] (Key) {

          if (!edit_mode && focused_message) {
            if (thread) {
              main_window->actions->doit (refptr<Action>(new ToggleAction(thread, "inbox")));
            }
          }

          return true;
        });

    keys.register_key ("C-P",
        "thread_view.print",
        "Print focused message",
        [&] (Key) {
        // TODO: [W2]
# if 0
          GError * err = NULL;
          WebKitDOMDocument * d = webkit_web_view_get_dom_document (webview);

          ustring mid = "message_" + focused_message->mid;
          WebKitDOMElement * e = webkit_dom_document_get_element_by_id (d, mid.c_str());
          WebKitDOMDOMTokenList * class_list =
            webkit_dom_element_get_class_list (WEBKIT_DOM_ELEMENT(e));

          webkit_dom_dom_token_list_add (class_list, "print",
              (err = NULL, &err));

          /* expand */
          bool wasexpanded = toggle_hidden (focused_message, ToggleShow);

          bool indented = indent_messages;
          if (indent_messages) {
            indent_messages = false;
            update_all_indent_states ();
          }

          /* open print window */
          WebKitWebFrame * frame = webkit_web_view_get_main_frame (webview);
          webkit_web_frame_print (frame);

          if (indented) {
            indent_messages = true;
            update_all_indent_states ();
          }

          if (!wasexpanded) {
            toggle_hidden (focused_message, ToggleHide);
          }

          webkit_dom_dom_token_list_remove (class_list, "print",
              (err = NULL, &err));

          g_object_unref (class_list);
          g_object_unref (e);
          g_object_unref (d);
# endif

          return true;
        });

    keys.register_key ("+",
        "thread_view.tag_message",
        "Tag message",
        [&] (Key) {
          if (edit_mode) {
            return false;
          }

          ustring tag_list = VectorUtils::concat_tags (focused_message->tags) + " ";

          main_window->enable_command (CommandBar::CommandMode::Tag,
              "Change tags for message:",
              tag_list,
              [&](ustring tgs) {
                LOG (debug) << "ti: got tags: " << tgs;

                vector<ustring> tags = VectorUtils::split_and_trim (tgs, ",| ");

                /* remove empty */
                tags.erase (std::remove (tags.begin (), tags.end (), ""), tags.end ());

                sort (tags.begin (), tags.end ());
                sort (focused_message->tags.begin (), focused_message->tags.end ());

                vector<ustring> rem;
                vector<ustring> add;

                /* find tags that have been removed */
                set_difference (focused_message->tags.begin (),
                                focused_message->tags.end (),
                                tags.begin (),
                                tags.end (),
                                std::back_inserter (rem));

                /* find tags that should be added */
                set_difference (tags.begin (),
                                tags.end (),
                                focused_message->tags.begin (),
                                focused_message->tags.end (),
                                std::back_inserter (add));


                if (add.size () == 0 &&
                    rem.size () == 0) {
                  LOG (debug) << "ti: nothing to do.";
                } else {
                  main_window->actions->doit (refptr<Action>(new TagAction (refptr<NotmuchItem>(new NotmuchMessage(focused_message)), add, rem)));
                }

                /* make sure that the unread tag is not modified after manually editing tags */
                state[focused_message].unread_checked = true;

              });
          return true;
        });

    keys.register_key ("C-f",
        "thread_view.search.search_or_next",
        "Search for text or go to next match",
        std::bind (&ThreadView::search, this, std::placeholders::_1, true));

    keys.register_key (UnboundKey (),
        "thread_view.search.search",
        "Search for text",
        std::bind (&ThreadView::search, this, std::placeholders::_1, false));


    keys.register_key (GDK_KEY_Escape, "thread_view.search.cancel",
        "Cancel current search",
        [&] (Key) {
          reset_search ();
          return true;
        });

    keys.register_key (UnboundKey (), "thread_view.search.next",
        "Go to next match",
        [&] (Key) {
          next_search_match ();
          return true;
        });

    keys.register_key ("P", "thread_view.search.previous",
        "Go to previous match",
        [&] (Key) {
          prev_search_match ();
          return true;
        });

    keys.register_key ("y", "thread_view.yank",
        "Yank current element or message text to clipboard",
        [&] (Key) {
          element_action (EYank);
          return true;
        });

    keys.register_key ("Y", "thread_view.yank_raw",
        "Yank raw content of current element or message to clipboard",
        [&] (Key) {
          element_action (EYankRaw);
          return true;
        });

    keys.register_key ("C-y", "thread_view.yank_mid",
        "Yank the Message-ID of the focused message to clipboard",
        [&] (Key) {
          auto cp = Gtk::Clipboard::get (astroid->clipboard_target);
          cp->set_text (focused_message->mid);

          LOG (info) << "tv: " << focused_message->mid << " copied to clipboard.";

          return true;
        });

    keys.register_key (Key (":"),
          "thread_view.multi_next_thread",
          "Open next after..",
          [&] (Key k) {
            multi_key (next_multi, k);

            return true;
          });

    next_multi.title = "Thread";
    next_multi.register_key (Key ("a"),
        "thread_view.multi_next_thread.archive",
        "Archive, goto next",
        [&] (Key) {
          keys.handle ("thread_view.archive_thread");
          emit_index_action (IA_Next);

          return true;
        });

    next_multi.register_key (Key ("A"),
        "thread_view.multi_next_thread.archive_next_unread_thread",
        "Archive, goto next unread",
        [&] (Key) {
          keys.handle ("thread_view.archive_thread");
          emit_index_action (IA_NextUnread);

          return true;
        });

    next_multi.register_key (Key ("x"),
        "thread_view.multi_next_thread.close",
        "Archive, close",
        [&] (Key) {
          keys.handle ("thread_view.archive_thread");
          close ();

          return true;
        });

    next_multi.register_key (Key ("j"),
        "thread_view.multi_next_thread.next_thread",
        "Goto next",
        [&] (Key) {
          emit_index_action (IA_Next);

          return true;
        });

    next_multi.register_key (Key ("k"),
        "thread_view.multi_next_thread.previous_thread",
        "Goto previous",
        [&] (Key) {
          emit_index_action (IA_Previous);

          return true;
        });

    next_multi.register_key (Key (GDK_KEY_Tab),
        "thread_view.multi_next_thread.next_unread",
        "Goto next unread",
        [&] (Key) {
          emit_index_action (IA_NextUnread);

          return true;
        });

    next_multi.register_key (Key (false, false, (guint) GDK_KEY_ISO_Left_Tab),
        "thread_view.multi_next_thread.previous_unread",
        "Goto previous unread",
        [&] (Key) {
          emit_index_action (IA_PreviousUnread);

          return true;
        });

    /* make aliases in main namespace */
    keys.register_key (UnboundKey (),
        "thread_view.archive_then_next",
        "Alias for thread_view.multi_next_thread.archive",
        [&] (Key) {
          return next_multi.handle ("thread_view.multi_next_thread.archive");
        });

    keys.register_key (UnboundKey (),
        "thread_view.archive_then_next_unread",
        "Alias for thread_view.multi_next_thread.archive_next_unread",
        [&] (Key) {
          return next_multi.handle ("thread_view.multi_next_thread.archive_next_unread_thread");
        });

    keys.register_key (UnboundKey (),
        "thread_view.archive_and_close",
        "Alias for thread_view.multi_next_thread.close",
        [&] (Key) {
          return next_multi.handle ("thread_view.multi_next_thread.close");
        });

    keys.register_key (UnboundKey (),
        "thread_view.next_thread",
        "Alias for thread_view.multi_next_thread.next_thread",
        [&] (Key) {
          return next_multi.handle ("thread_view.multi_next_thread.next_thread");
        });

    keys.register_key (UnboundKey (),
        "thread_view.previous_thread",
        "Alias for thread_view.multi_next_thread.previous_thread",
        [&] (Key) {
          return next_multi.handle ("thread_view.multi_next_thread.previous_thread");
        });

    keys.register_key (UnboundKey (),
        "thread_view.next_unread_thread",
        "Alias for thread_view.multi_next_thread.next_unread",
        [&] (Key) {
          return next_multi.handle ("thread_view.multi_next_thread.next_unread");
        });

    keys.register_key (UnboundKey (),
        "thread_view.previous_unread_thread",
        "Alias for thread_view.multi_next_thread.previous_unread",
        [&] (Key) {
          return next_multi.handle ("thread_view.multi_next_thread.previous_unread");
        });


  } // }}}

  bool ThreadView::element_action (ElementAction a) { // {{{
    LOG (debug) << "tv: activate item.";

    if (!(focused_message)) {
      LOG (error) << "tv: no message has focus yet.";
      return true;
    }

    if (!edit_mode && !state[focused_message].expanded) {
      if (a == EEnter) {
        toggle (focused_message);
      } else if (a == ESave) {
        /* save message to */
        focused_message->save ();

      } else if (a == EYankRaw) {
        auto    cp = Gtk::Clipboard::get (astroid->clipboard_target);
        ustring t  = "";

        auto d   = focused_message->raw_contents ();
        auto cnv = UstringUtils::bytearray_to_ustring (d);
        if (cnv.first) {
          t = UstringUtils::unixify (cnv.second);
        }

        cp->set_text (t);

      } else if (a == EYank) {

        auto cp = Gtk::Clipboard::get (astroid->clipboard_target);
        ustring t;

        t = focused_message->viewable_text (false, true);

        cp->set_text (t);
      }

    } else {
      if (state[focused_message].current_element == 0) {
        if (!edit_mode && a == EEnter) {
          /* nothing selected, closing message */
          toggle (focused_message);
        } else if (a == ESave) {
          /* save message to */
          focused_message->save ();
        } else if (a == EYankRaw) {
          auto    cp = Gtk::Clipboard::get (astroid->clipboard_target);
          ustring t  = "";

          auto d = focused_message->raw_contents ();
          auto cnv = UstringUtils::bytearray_to_ustring (d);
          if (cnv.first) {
            t = UstringUtils::unixify (cnv.second);
          }

          cp->set_text (t);

        } else if (a == EYank) {

          auto cp = Gtk::Clipboard::get (astroid->clipboard_target);
          ustring t;

          t = focused_message->viewable_text (false, true);

          cp->set_text (t);
        }

      } else {
        if (a == EYankRaw) {

          refptr<Chunk> c = focused_message->get_chunk_by_id (
              state[focused_message].elements[state[focused_message].current_element].id);
          auto cp = Gtk::Clipboard::get (astroid->clipboard_target);
          ustring t = "";

          auto d   = c->contents ();
          auto cnv = UstringUtils::bytearray_to_ustring (d);
          if (cnv.first) {
            t = cnv.second;
          }

          cp->set_text (t);

        } else if (a == EYank) {

          refptr<Chunk> c = focused_message->get_chunk_by_id (
              state[focused_message].elements[state[focused_message].current_element].id);
          auto cp = Gtk::Clipboard::get (astroid->clipboard_target);
          ustring t;

          if (c->viewable) {
            t = c->viewable_text (false, false);
          } else {
            LOG (error) << "tv: cannot yank text of non-viewable part";
          }

          cp->set_text (t);

        } else {

          switch (state[focused_message].elements[state[focused_message].current_element].type) {
            case MessageState::ElementType::Attachment:
              {
                if (a == EEnter) {
                  /* open attachment */

                  refptr<Chunk> c = focused_message->get_chunk_by_id (
                      state[focused_message].elements[state[focused_message].current_element].id);

                  if (c) {
                    c->open ();
                  } else {
                    LOG (error) << "tv: could not find chunk for element.";
                  }

                } else if (a == ESave) {
                  /* save attachment */
                  refptr<Chunk> c = focused_message->get_chunk_by_id (
                      state[focused_message].elements[state[focused_message].current_element].id);

                  if (c) {
                    c->save ();
                  } else {
                    LOG (error) << "tv: could not find chunk for element.";
                  }
                }
              }
              break;
            case MessageState::ElementType::Part:
              {
                if (a == EEnter) {
                  /* open part */

                  refptr<Chunk> c = focused_message->get_chunk_by_id (
                      state[focused_message].elements[state[focused_message].current_element].id);

                  if (c) {
                    if (open_html_part_external) {
                      c->open ();
                    } else {
                      /* show part internally */

                      display_part (focused_message, c, state[focused_message].elements[state[focused_message].current_element]);

                    }
                  } else {
                    LOG (error) << "tv: could not find chunk for element.";
                  }

                } else if (a == ESave) {
                  /* save part */
                  refptr<Chunk> c = focused_message->get_chunk_by_id (
                      state[focused_message].elements[state[focused_message].current_element].id);

                  if (c) {
                    c->save ();
                  } else {
                    LOG (error) << "tv: could not find chunk for element.";
                  }
                }

              }
              break;

            case MessageState::ElementType::MimeMessage:
              {
                if (a == EEnter) {
                  /* open part */
                  refptr<Chunk> c = focused_message->get_chunk_by_id (
                      state[focused_message].elements[state[focused_message].current_element].id);

                  refptr<MessageThread> mt = refptr<MessageThread> (new MessageThread ());
                  mt->add_message (c);

                  ThreadView * tv = new ThreadView (main_window);
                  tv->load_message_thread (mt);

                  main_window->add_mode (tv);

                } else if (a == ESave) {
                  /* save part */
                  refptr<Chunk> c = focused_message->get_chunk_by_id (
                      state[focused_message].elements[state[focused_message].current_element].id);

                  if (c) {
                    c->save ();
                  } else {
                    LOG (error) << "tv: could not find chunk for element.";
                  }
                }
              }
              break;

            default:
              break;
          }
        }
      }
    }

    if (state[focused_message].current_element > 0) {
      emit_element_action (state[focused_message].current_element, a);
    }
    return true;
  } // }}}

  bool ThreadView::unread_check () {
    if (!ready) {
      unread_setup = false;
      return false; // disconnect
    }

    if (!edit_mode && focused_message && focused_message->in_notmuch) {

      if (!state[focused_message].unread_checked && state[focused_message].expanded) {

        chrono::duration<double> elapsed = chrono::steady_clock::now() - focus_time;

        if (unread_delay == 0.0 || elapsed.count () > unread_delay) {
          if (focused_message->has_tag ("unread")) {

            main_window->actions->doit (refptr<Action>(new TagAction (refptr<NotmuchItem>(new NotmuchMessage(focused_message)), {}, { "unread" })), false);
            state[focused_message].unread_checked = true;
          }
        }
      }
    }

    return true;
  }

  void ThreadView::focus_change_cb (std::string e) {
    LOG (debug) << "tv: got focus_change, element: " << e;

    auto vs = VectorUtils::split_and_trim (e, ",");
    for (auto m : mthread->messages) {
      if (vs[0] == m->safe_mid ()) {
        focused_message = m;
        if (vs[0] != vs[1]) {
          unsigned int i = 0;
          for (auto e : state[m].elements) {
            if (e.element_id () == vs[1]) {
              state[m].current_element = i;
            }
            i++;
          }

        } else {
          state[m].current_element = 0;
        }
      }
    }

    /* focused_message = m; */
    /* state[focused_message].current_element = e; */
  }

  void ThreadView::focus_next_element (bool force_change) {
    /*
     * Jump to next element (if no scrolling is necessary),
     * otherwise scroll the viewport a small increment.
     *
     * If force_change is true, then always move focus to the next element.
     *
     * The function should return the currently selected message and element.
     *
     */

    LOG (info) << "tv: focus next element, force_change=" << force_change;

    string js = "Astroid.focus_next_element(" + ( force_change ? string("true") : string("false")) + ");";


    /* run_javascript (js, std::bind (&ThreadView::focus_change_cb, this, std::placeholders::_1)); */
  }

  // TODO: [JS]
  void ThreadView::focus_previous_element (bool force_change) {
    /* Inverse of focus_next_element */

    LOG (debug) << "tv: focus previous element, force_change=" << force_change;

    string js = "Astroid.focus_previous_element(" + ( force_change ? string("true") : string("false")) + ");";
    /* run_javascript (js, std::bind (&ThreadView::focus_change_cb, this, std::placeholders::_1)); */
  }

  void ThreadView::focus_element (refptr<Message> m, unsigned int e) {
    LOG (debug) << "tv: focus element, e=" << e;

    string js = ustring::compose ("Astroid.focus_element (%1, %2);",
        m->safe_mid (), e);

    // TODO [JS]
    LOG (error) << "tv: focus element NOT IMPLEMENTED!";
    /* run_javascript (js, std::bind (&ThreadView::focus_change_cb, this, std::placeholders::_1)); */
  }

  void ThreadView::focus_message (refptr<Message> m) {
    focus_element (m, 0);
  }

  void ThreadView::focus_next_message () {
    LOG (debug) << "tv: focus_next_message";

    if (edit_mode) return;

    int focused_position = find (
        mthread->messages.begin (),
        mthread->messages.end (),
        focused_message) - mthread->messages.begin ();

    if (focused_position < static_cast<int>((mthread->messages.size ()-1))) {
      focus_message (mthread->messages[focused_position + 1]);
    }

  }

  void ThreadView::focus_previous_message (bool focus_top) {
    LOG (debug) << "tv: focus_previous_message";
    if (edit_mode) return;

    int focused_position = find (
        mthread->messages.begin (),
        mthread->messages.end (),
        focused_message) - mthread->messages.begin ();

    if (focused_position > 0) {
      auto m = mthread->messages[focused_position - 1];
      if (!focus_top && state[focused_message].expanded) {
        focus_element (m, state[m].elements.size ()-1); // start at bottom
      } else {
        focus_message (m); // start at top
      }
    }
  }

  /* end focus handeling   */

  /* message expanding and collapsing  */
  bool ThreadView::expand (refptr<Message> m) {
    /* returns true if the message was expanded in the first place */

    bool wasexpanded  = state[m].expanded;

    if (!wasexpanded) {
      state[m].expanded = true;
      std::string js = "Astroid.expand_message ('" + m->safe_mid () + "');";
      /* run_javascript (js); */

      /* if the message was unexpanded, it would not have been marked as read */
      if (unread_delay == 0.0) unread_check ();
    }

    return wasexpanded;
  }

  bool ThreadView::collapse (refptr<Message> m) {
    /* returns true if the message was expanded in the first place */
    bool wasexpanded  = state[m].expanded;

    if (wasexpanded) {
      state[m].expanded = false;
      std::string js = "Astroid.collapse_message ('" + m->safe_mid () + "');";
      /* run_javascript (js); */
    }

    return wasexpanded;
  }

  bool ThreadView::toggle (refptr<Message> m)
  {
    /* returns true if the message was expanded in the first place */
    if (state[m].expanded) return collapse (m);
    else                   return expand (m);
  }

  /* end message expanding and collapsing  */

  void ThreadView::save_all_attachments () { //
    /* save all attachments of current focused message */
    LOG (info) << "tv: save all attachments..";

    if (!focused_message) {
      LOG (warn) << "tv: no message focused!";
      return;
    }

    auto attachments = focused_message->attachments ();
    if (attachments.empty ()) {
      LOG (warn) << "tv: this message has no attachments to save.";
      return;
    }

    Gtk::FileChooserDialog dialog ("Save attachments to folder..",
        Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);

    dialog.add_button ("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button ("_Select", Gtk::RESPONSE_OK);
    dialog.set_current_folder (astroid->runtime_paths ().save_dir.c_str ());

    int result = dialog.run ();

    switch (result) {
      case (Gtk::RESPONSE_OK):
        {
          string dir = dialog.get_filename ();
          LOG (info) << "tv: saving attachments to: " << dir;

          astroid->runtime_paths ().save_dir = bfs::path (dialog.get_current_folder ());

          /* TODO: check if the file exists and ask to overwrite. currently
           *       we are failing silently (except an error message in the log)
           */
          for (refptr<Chunk> a : attachments) {
            a->save_to (dir);
          }

          break;
        }

      default:
        {
          LOG (debug) << "tv: save: cancelled.";
        }
    }
  } //

  /* general mode stuff  */
  void ThreadView::grab_focus () {
    //LOG (debug) << "tv: grab focus";
    gtk_widget_grab_focus (GTK_WIDGET (webview));
  }

  void ThreadView::grab_modal () {
    add_modal_grab ();
    grab_focus ();

    //gtk_grab_add (GTK_WIDGET (webview));
    //gtk_widget_grab_focus (GTK_WIDGET (webview));
  }

  void ThreadView::release_modal () {
    remove_modal_grab ();
    //gtk_grab_remove (GTK_WIDGET (webview));
  }

  /* end general mode stuff  */

  /* signals  */
  ThreadView::type_signal_ready
    ThreadView::signal_ready ()
  {
    return m_signal_ready;
  }

  void ThreadView::emit_ready () {
    LOG (info) << "tv: ready emitted.";
    ready = true;
    m_signal_ready.emit ();
  }

  ThreadView::type_element_action
    ThreadView::signal_element_action ()
  {
    return m_element_action;
  }

  void ThreadView::emit_element_action (unsigned int element, ElementAction action) {
    LOG (debug) << "tv: element action emitted: " << element << ", action: enter";
    m_element_action.emit (element, action);
  }

  ThreadView::type_index_action ThreadView::signal_index_action () {
    return m_index_action;
  }

  bool ThreadView::emit_index_action (IndexAction action) {
    LOG (debug) << "tv: index action: " << action;
    return m_index_action.emit (this, action);
  }

  /* end signals  */

  /* MessageState   */
  ThreadView::MessageState::MessageState () {
    elements.push_back (Element (Empty, -1));
    current_element = 0;
  }

  ThreadView::MessageState::Element::Element (ThreadView::MessageState::ElementType t, int i)
  {
    type = t;
    id   = i;
  }

  bool ThreadView::MessageState::Element::operator== ( const Element & other ) const {
    return other.id == id;
  }

  ustring ThreadView::MessageState::Element::element_id () {
    return ustring::compose("%1", id);
  }

  ThreadView::MessageState::Element * ThreadView::MessageState::get_current_element () {
    if (current_element == 0) {
      return NULL;
    } else {
      return &(elements[current_element]);
    }
  }

  /* end MessageState  */

  /* Searching  */
  bool ThreadView::search (Key, bool next) {
    if (in_search && next) {
      next_search_match ();
      return true;
    }

    reset_search ();

    main_window->enable_command (CommandBar::CommandMode::SearchText,
        "", sigc::mem_fun (this, &ThreadView::on_search));

    return true;
  }

  void ThreadView::reset_search () {
    /* reset */
    if (in_search) {
      /* reset search expanded state */
      for (auto m : mthread->messages) {
        state[m].search_expanded = false;
      }
    }

    in_search = false;
    in_search_match = false;
    search_q  = "";

    // TODO: [W2]
# if 0
    webkit_web_view_set_highlight_text_matches (webview, false);
    webkit_web_view_unmark_text_matches (webview);
# endif
  }

  void ThreadView::on_search (ustring k) {
    reset_search ();

    if (!k.empty ()) {

      /* expand all messages, these should be closed - except the focused one
       * when a search is cancelled */
      for (auto m : mthread->messages) {
        state[m].search_expanded = !expand (m);
      }

      LOG (debug) << "tv: searching for: " << k;
      // TODO: [W2]
      /* int n = webkit_web_view_mark_text_matches (webview, k.c_str (), false, 0); */
      int n = 0; // TODO

      LOG (debug) << "tv: search, found: " << n << " matches.";

      in_search = (n > 0);

      if (in_search) {
        search_q = k;

        // TODO: [W2]
        /* webkit_web_view_set_highlight_text_matches (webview, true); */

        next_search_match ();

      } else {
        /* un-expand messages again */
        for (auto m : mthread->messages) {
          if (state[m].search_expanded) collapse (m);
          state[m].search_expanded = false;
        }

      }
    }
  }

  void ThreadView::next_search_match () {
    if (!in_search) return;
    /* there does not seem to be a way to figure out which message currently
     * contains the selected matched text, but when there is a scroll event
     * the match is centered.
     *
     * the focusing is handled in on_scroll_vadjustment...
     *
     */

    in_search_match = true;
    // TODO: [W2]
    /* webkit_web_view_search_text (webview, search_q.c_str (), false, true, true); */
  }

  void ThreadView::prev_search_match () {
    if (!in_search) return;

    in_search_match = true;
    // TODO: [W2]
    /* webkit_web_view_search_text (webview, search_q.c_str (), false, false, true); */
  }

  /* Utils */
  std::string ThreadView::assemble_data_uri (ustring mime_type, gchar * &data, gsize len) {

    std::string base64 = "data:" + mime_type + ";base64," + Glib::Base64::encode (std::string(data, len));

    return base64;
  }

  /***************
   * Exceptions
   ***************/

  webkit_error::webkit_error (const char * w) : std::runtime_error (w)
  {
  }
}

