# include "page_client.hh"
# include "utils/resource.hh"

# include <webkit2/webkit2.h>
# include <unistd.h>
# include <gtkmm.h>
# include <glib.h>
# include <glibmm.h>
# include <giomm.h>
# include <giomm/socket.h>
# include <boost/filesystem.hpp>
# include <iostream>
# include <thread>

# include "astroid.hh"
# include "modes/thread_view/webextension/ae_protocol.hh"
# include "modes/thread_view/webextension/dom_utils.hh"
# include "messages.pb.h"

# include "astroid.hh"
# include "config.hh"
# include "thread_view.hh"
# include "message_thread.hh"
# include "chunk.hh"
# include "utils/utils.hh"
# include "utils/address.hh"
# include "utils/vector_utils.hh"
# include "utils/ustring_utils.hh"
# include "utils/gravatar.hh"

# ifndef DISABLE_PLUGINS
  # include "plugin/manager.hh"
# endif

using namespace boost::filesystem;

namespace Astroid {
  int PageClient::id = 0;

  PageClient::PageClient () {

    id++;
    ready = false;

    /* load attachment icon */
    Glib::RefPtr<Gtk::IconTheme> theme = Gtk::IconTheme::get_default();
    attachment_icon = theme->load_icon (
        "mail-attachment-symbolic",
        ATTACHMENT_ICON_WIDTH,
        Gtk::ICON_LOOKUP_USE_BUILTIN );

    extension_connect_id = g_signal_connect (webkit_web_context_get_default (),
        "initialize-web-extensions",
        G_CALLBACK (PageClient_init_web_extensions),
        (gpointer) this);

  }

  extern "C" void PageClient_init_web_extensions (
      WebKitWebContext * context,
      gpointer           user_data) {

    ((PageClient *) user_data)->init_web_extensions (context);
  }

  PageClient::~PageClient () {
    LOG (debug) << "pc: destruct";
    g_signal_handler_disconnect (webkit_web_context_get_default (),
        extension_connect_id);

    reader_run = false;

    LOG (debug) << "pc: closing";

    if (reader_cancel)
      reader_cancel->cancel ();
    reader_t.join ();

    istream.clear ();
    ostream.clear ();

    ext->close ();
    srv->close ();
  }

  void PageClient::init_web_extensions (WebKitWebContext * context) {

    /* add path to Astroid web extension */
# ifdef DEBUG
    LOG (warn) << "pc: adding " << Resource::get_exe_dir ().c_str () << " to web extension search path.";

    webkit_web_context_set_web_extensions_directory (
        context,
        Resource::get_exe_dir().c_str());

# else
    // TODO: Add install path
# endif

    /* set up unix socket */
    LOG (warn) << "pc: id: " << id;

    // TODO: do-while socket_addr path exists
    socket_addr = ustring::compose ("%1/sockets/astroid.%2.%3",
        astroid->standard_paths ().runtime_dir.c_str(),
        id,
        UstringUtils::random_alphanumeric (30));

    refptr<Gio::UnixSocketAddress> addr = Gio::UnixSocketAddress::create (socket_addr,
        Gio::UNIX_SOCKET_ADDRESS_ABSTRACT);

    refptr<Gio::SocketAddress> eaddr;

    LOG (debug) << "pc: socket: " << addr->get_path ();

    mode_t p = umask (0077);
    srv = Gio::SocketListener::create ();
    srv->add_address (addr, Gio::SocketType::SOCKET_TYPE_STREAM,
      Gio::SocketProtocol::SOCKET_PROTOCOL_DEFAULT,
      eaddr);

    /* listen */
    srv->accept_async (sigc::mem_fun (this, &PageClient::extension_connect));
    umask (p);

    /* send socket address (TODO: include key) */
    GVariant * gaddr = g_variant_new_string (addr->get_path ().c_str ());

    webkit_web_context_set_web_extensions_initialization_user_data (
        context,
        gaddr);
  }

  void PageClient::extension_connect (refptr<Gio::AsyncResult> &res) {
    LOG (warn) << "pc: got extension connect";

    ext = refptr<Gio::UnixConnection>::cast_dynamic (srv->accept_finish (res));

    istream = ext->get_input_stream ();
    ostream = ext->get_output_stream ();

    /* setting up reader */
    reader_cancel = Gio::Cancellable::create ();
    reader_t = std::thread (&PageClient::reader, this);

    ready = true;

    if (thread_view->wk_loaded) {
      load ();
      thread_view->render_messages ();

      /* usually this will be called from thread_view, but extension may not yet be ready */
    }
  }

  void PageClient::reader () {
    while (reader_run) {
      gsize read = 0;

      /* read size of message */
      gsize sz;
      try {
        read = istream->read ((char*)&sz, sizeof (sz), reader_cancel); // blocking
      } catch (Gio::Error &ex) {
        LOG (warn) << "pc: " << ex.what ();
        reader_run = false;
        return;
      }

      if (read != sizeof(sz)) break;;

      /* read message type */
      AeProtocol::MessageTypes mt;
      read = istream->read ((char*)&mt, sizeof (mt));
      if (read != sizeof (mt)) break;

      /* read message */
      gchar buffer[sz + 1]; buffer[sz] = '\0'; // TODO: set max buffer size
      bool s = istream->read_all (buffer, sz, read);

      if (!s) break;

      /* parse message */
      switch (mt) {
        case AeProtocol::MessageTypes::Debug:
          {
            AstroidMessages::Debug m;
            m.ParseFromString (buffer);
            LOG (debug) << "pc: ae: " << m.msg ();
          }
          break;

        default:
          break; // unknown message
      }
    }
  }

  void PageClient::load () {
    /* load style sheet */
    LOG (debug) << "pc: sending stylesheet..";
    AstroidMessages::StyleSheet s;
    s.set_css (thread_view->theme.thread_view_css.c_str ());
    AeProtocol::send_message (AeProtocol::MessageTypes::StyleSheet, s, ostream);
  }

  void PageClient::write () {
    AstroidMessages::Debug m;
    m.set_msg ("here is the message!");

    AeProtocol::send_message (AeProtocol::MessageTypes::Debug, m, ostream);
  }

  void PageClient::update_state () {
    /* Make state structure */
    AstroidMessages::State state;

    state.set_focused (thread_view->focused_message->safe_mid ());

    for (auto &ms : thread_view->state) {
      AstroidMessages::State::MessageState * m = state.add_messages ();

      m->set_mid (ms.first->safe_mid ());
      m->set_marked (ms.second.marked);
      m->set_expanded (ms.second.expanded);

      for (auto &e : ms.second.elements) {
        AstroidMessages::State::MessageState::Element * _e = m->add_elements ();

        auto ref = _e->GetReflection();
        ref->SetEnumValue (_e, _e->GetDescriptor()->FindFieldByName("type"), e.type);
        _e->set_id (e.id);
        _e->set_element_id (e.element_id ());
      }
    }

    AeProtocol::send_message (AeProtocol::MessageTypes::State, state, ostream);
  }

  void PageClient::set_marked_state (refptr<Message> m, bool marked) {
    AstroidMessages::Mark msg;
    msg.set_mid (m->safe_mid ());
    msg.set_marked (marked);

    AeProtocol::send_message (AeProtocol::MessageTypes::Mark, msg, ostream);
  }

  void PageClient::set_hidden_state (refptr<Message> m, bool hidden) {
    LOG (debug) << "pc: set hidden state";
    AstroidMessages::Hidden msg;
    msg.set_mid (m->safe_mid ());
    msg.set_hidden (hidden);

    AeProtocol::send_message (AeProtocol::MessageTypes::Hidden, msg, ostream);
  }

  void PageClient::set_focus (refptr<Message> m, unsigned int e) {
    if (m) {
      LOG (debug) << "pc: focusing: " << m->safe_mid () << ": " << e;
      AstroidMessages::Focus msg;
      msg.set_mid (m->safe_mid ());
      msg.set_focus (true);
      msg.set_element (e);

      AeProtocol::send_message (AeProtocol::MessageTypes::Focus, msg, ostream);
    } else {
      LOG (warn) << "pc: tried to focus unset message";
    }
  }

  void PageClient::add_message (refptr<Message> m) {
    AeProtocol::send_message (AeProtocol::MessageTypes::AddMessage, make_message (m), ostream);
  }

  void PageClient::update_message (refptr<Message> m) {
    AeProtocol::send_message (AeProtocol::MessageTypes::UpdateMessage, make_message (m), ostream);
  }

  AstroidMessages::Message PageClient::make_message (refptr<Message> m) {
    AstroidMessages::Message msg;

    msg.set_mid (m->safe_mid());

    Address sender (m->sender);
    msg.mutable_sender()->set_name (sender.fail_safe_name ());
    msg.mutable_sender()->set_email (sender.email ());
    msg.mutable_sender ()->set_full_address (sender.full_address ());

    for (Address &recipient: AddressList(m->to()).addresses) {
      AstroidMessages::Address * a = msg.mutable_to()->add_addresses();
      a->set_name (recipient.fail_safe_name ());
      a->set_email (recipient.email ());
      a->set_full_address (recipient.full_address ());
    }

    for (Address &recipient: AddressList(m->cc()).addresses) {
      AstroidMessages::Address * a = msg.mutable_cc()->add_addresses();
      a->set_name (recipient.fail_safe_name ());
      a->set_email (recipient.email ());
      a->set_full_address (recipient.full_address ());
    }

    for (Address &recipient: AddressList(m->bcc()).addresses) {
      AstroidMessages::Address * a = msg.mutable_bcc()->add_addresses();
      a->set_name (recipient.fail_safe_name ());
      a->set_email (recipient.email ());
      a->set_full_address (recipient.full_address ());
    }

    msg.set_date_pretty (m->pretty_date ());
    msg.set_date_verbose (m->pretty_verbose_date (true));

    msg.set_subject (m->subject);

    for (ustring &tag : m->tags) {
      AstroidMessages::Tag * t = msg.add_tags ();
      t->set_tag (tag);

      unsigned char cv[] = { 0xff, 0xff, 0xff }; // assuming tag-background is white
      auto clrs = Utils::get_tag_color (tag, cv);
      t->set_fg (clrs.first);
      t->set_bg (clrs.second);
    }

    /* avatar */
    {
      ustring uri = "";
      auto se = Address(m->sender);
# ifdef DISABLE_PLUGINS
      if (false) {
# else
      if (thread_view->plugins->get_avatar_uri (se.email (), Gravatar::DefaultStr[Gravatar::Default::RETRO], 48, m, uri)) {
# endif
        ; // all fine, use plugins avatar
      } else {
        if (enable_gravatar) {
          uri = Gravatar::get_image_uri (se.email (),Gravatar::Default::RETRO , 48);
        }
      }

      msg.set_gravatar (uri);
    }

    return msg;
  }

  ustring PageClient::get_attachment_thumbnail (refptr<Chunk> c) { // {{{
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

    return DomUtils::assemble_data_uri (image_content_type, content, content_size);
  } // }}}
}

