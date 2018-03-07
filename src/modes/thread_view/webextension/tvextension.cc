# include "tvextension.hh"

# include <webkit2/webkit-web-extension.h>

# include <gmodule.h>
# include <iostream>
# include <glib.h>
# include <glibmm.h>
# include <giomm.h>
# include <giomm/socket.h>
# include <thread>

# include "modes/thread_view/webextension/ae_protocol.hh"
# include "modes/thread_view/webextension/dom_utils.hh"
# include "messages.pb.h"


using std::cout;
using std::endl;

using namespace Astroid;

extern "C" {

static void
web_page_created_callback (WebKitWebExtension *extension,
                           WebKitWebPage      *web_page,
                           gpointer            user_data )
{
  ext->page_created (extension, web_page, user_data);
}

G_MODULE_EXPORT void
webkit_web_extension_initialize_with_user_data (
    WebKitWebExtension *extension,
    gpointer pipes)
{
  ext = new AstroidExtension (extension, pipes);

  g_signal_connect (extension, "page-created",
      G_CALLBACK (web_page_created_callback),
      NULL);

}
}

AstroidExtension::AstroidExtension (WebKitWebExtension * e,
    gpointer gaddr) {
  extension = e;

  std::cout << "ae: inititalize" << std::endl;

  Gio::init ();

  /* set up theme */


  /* retrieve socket address */
  std::cout << "type: " << g_variant_get_type_string ((GVariant *)gaddr) << std::endl;

  gsize sz;
  const char * caddr = g_variant_get_string ((GVariant *) gaddr, &sz);
  std::cout << "addr: " << caddr << std::endl;

  refptr<Gio::UnixSocketAddress> addr = Gio::UnixSocketAddress::create (caddr,
      Gio::UNIX_SOCKET_ADDRESS_PATH);

  /* connect to socket */
  std::cout << "ae: connecting.." << std::endl;
  cli = Gio::SocketClient::create ();

  try {
    sock = cli->connect (addr);

    istream = sock->get_input_stream ();
    ostream = sock->get_output_stream ();

    /* setting up reader thread */
    reader_t = std::thread (&AstroidExtension::reader, this);

  } catch (Gio::Error &ex) {
    cout << "ae: error: " << ex.what () << endl;
  }

  std::cout << "ae: init done" << std::endl;
}

void AstroidExtension::reader () {
  cout << "ae: reader thread: started." << endl;

  while (run) {
    gchar buffer[2049]; buffer[0] = '\0';
    gsize read = 0;

    /* read size of message */
    gsize sz;
    read = istream->read ((char*)&sz, sizeof (sz));

    if (read != sizeof(sz)) break;;

    /* read message type */
    AeProtocol::MessageTypes mt;
    read = istream->read ((char*)&mt, sizeof (mt));
    if (read != sizeof (mt)) break;

    /* read message */
    bool s = istream->read_all (buffer, sz, read);

    if (!s) break;

    /* parse message */
    switch (mt) {
      case AeProtocol::MessageTypes::Debug:
        {
          AstroidMessages::Debug m;
          m.ParseFromString (buffer);
          cout << "ae: " << m.msg () << endl;
        }
        break;

      case AeProtocol::MessageTypes::Mark:
        {
          AstroidMessages::Mark m;
          m.ParseFromString (buffer);
          handle_mark (m);
        }
        break;

      case AeProtocol::MessageTypes::AddMessage:
        {
          AstroidMessages::Message m;
          m.ParseFromString (buffer);
          add_message (m);
        }
        break;

      default:
        break; // unknown message
    }
  }
}

void AstroidExtension::handle_mark (AstroidMessages::Mark &m) {
  GError *err;
  ustring mid = "message_" + m.mid();

  WebKitDOMDocument * d = webkit_web_page_get_dom_document (page);

  WebKitDOMElement * e = webkit_dom_document_get_element_by_id (d, mid.c_str());

  WebKitDOMDOMTokenList * class_list =
    webkit_dom_element_get_class_list (e);

  /* set class  */
  if (m.marked()) {
    webkit_dom_dom_token_list_add (class_list, (err = NULL, &err), "marked");
  } else {
    webkit_dom_dom_token_list_remove (class_list, (err = NULL, &err), "marked");
  }

  g_object_unref (class_list);
  g_object_unref (e);
  g_object_unref (d);
}

void AstroidExtension::load_page () {
  WebKitDOMDocument *d = webkit_web_page_get_dom_document (page);

  /* get container for message divs */
  container = WEBKIT_DOM_HTML_DIV_ELEMENT(webkit_dom_document_get_element_by_id (d, "message_container"));

  g_object_unref (d);
}

void AstroidExtension::add_message (AstroidMessages::Message &m) {
  ustring div_id = "message_" + m.mid();

  WebKitDOMNode * insert_before = webkit_dom_node_get_last_child (
      WEBKIT_DOM_NODE (container));

  WebKitDOMHTMLElement * div_message = DomUtils::make_message_div (page);

  GError * err = NULL;
  webkit_dom_element_set_attribute (WEBKIT_DOM_ELEMENT(div_message),
      "id", div_id.c_str(), &err);

  /* insert message div */
  webkit_dom_node_insert_before (WEBKIT_DOM_NODE(container),
      WEBKIT_DOM_NODE(div_message),
      insert_before,
      (err = NULL, &err));

  set_message_html (m, div_message);

  /* insert mime messages */
  if (!m.missing_content()) {
    insert_mime_messages (m, div_message);
  }

  /* insert attachments */
  if (!m.missing_content()) {
    insert_attachments (m, div_message);
  }

  /* marked */
  load_marked_icon (m, div_message);


  g_object_unref (insert_before);
  g_object_unref (div_message);
  g_object_unref (container);
}


/* main message generation  */
void AstroidExtension::set_message_html (
    AstroidMessages::Message m,
    WebKitDOMHTMLElement * div_message)
{
  GError *err;

  /* load message into div */
  ustring header;
  WebKitDOMHTMLElement * div_email_container =
    DomUtils::select (WEBKIT_DOM_NODE(div_message), "div.email_container");

  /* build header */
  insert_header_address (header, "From", m.sender(), true);

  if (m.reply_to().email().size () > 0) {
    if (m.reply_to().email() != m.sender().email())
      insert_header_address (header, "Reply-To", m.reply_to(), false);
  }

  insert_header_address_list (header, "To", m.to(), false);

  if (m.cc().addresses().size () > 0) {
    insert_header_address_list (header, "Cc", m.cc(), false);
  }

  if (m.bcc().addresses().size () > 0) {
    insert_header_address_list (header, "Bcc", m.bcc(), false);
  }

  insert_header_date (header, m);

  if (m.subject().size() > 0) {
    insert_header_row (header, "Subject", m.subject(), false);

    WebKitDOMHTMLElement * subject = DomUtils::select (
        WEBKIT_DOM_NODE (div_message),
        ".header_container .subject");

    ustring s = Glib::Markup::escape_text(m.subject());
    if (static_cast<int>(s.size()) > MAX_PREVIEW_LEN)
      s = s.substr(0, MAX_PREVIEW_LEN - 3) + "...";

    webkit_dom_element_set_inner_html (WEBKIT_DOM_ELEMENT (subject), s.c_str(), (err = NULL, &err));

    g_object_unref (subject);
  }

  if (m.tags().size () > 0) {
    header += create_header_row ("Tags", "", false, false, true);
  }

  /* avatar */
  /*
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
      if (enable_gravatar) {
        uri = Gravatar::get_image_uri (se.email (),Gravatar::Default::RETRO , 48);
      }
    }

    if (!uri.empty ()) {
      WebKitDOMHTMLImageElement * av = WEBKIT_DOM_HTML_IMAGE_ELEMENT (
          DomUtils::select (
          WEBKIT_DOM_NODE (div_message),
          ".avatar"));

      webkit_dom_element_set_attribute (WEBKIT_DOM_ELEMENT (av), "src",
          uri.c_str (),
          (err = NULL, &err));

      g_object_unref (av);
    }
  }
    */

  /* insert header html*/
  WebKitDOMHTMLElement * table_header =
    DomUtils::select (WEBKIT_DOM_NODE(div_email_container),
        ".header_container .header");

  webkit_dom_element_set_inner_html (
      WEBKIT_DOM_ELEMENT(table_header),
      header.c_str(),
      (err = NULL, &err));

  /* message_render_tags (m, WEBKIT_DOM_ELEMENT(div_message)); */
  /* message_update_css_tags (m, WEBKIT_DOM_ELEMENT(div_message)); */

  /* if message is missing body, set warning and don't add any content */

  WebKitDOMHTMLElement * span_body =
    DomUtils::select (WEBKIT_DOM_NODE(div_email_container), ".body");

  WebKitDOMHTMLElement * preview = DomUtils::select (
      WEBKIT_DOM_NODE (div_message),
      ".header_container .preview");

  if (m.missing_content()) {
    /* set preview */
    webkit_dom_element_set_inner_html (WEBKIT_DOM_ELEMENT(preview), "<i>Message content is missing.</i>", (err = NULL, &err));

    /* set warning */
    set_warning (m, "The message file is missing, only fields cached in the notmuch database are shown. Most likely your database is out of sync.");

    /* add an explenation to the body */
    GError *err;

    WebKitDOMDocument * d = webkit_web_page_get_dom_document (page);
    WebKitDOMHTMLElement * body_container =
      DomUtils::clone_select (WEBKIT_DOM_NODE(d), "#body_template");

    webkit_dom_element_remove_attribute (WEBKIT_DOM_ELEMENT (body_container),
        "id");

    webkit_dom_element_set_inner_html (
        WEBKIT_DOM_ELEMENT(body_container),
        "<i>Message content is missing.</i>",
        (err = NULL, &err));

    webkit_dom_node_append_child (WEBKIT_DOM_NODE (span_body),
        WEBKIT_DOM_NODE (body_container), (err = NULL, &err));

    g_object_unref (body_container);
    g_object_unref (d);

  } else {

    /* build message body */
    /* create_message_part_html (m, m->root, span_body, true); */

    /* preview */
    /* LOG (debug) << "tv: make preview.."; */

    /*
    ustring bp = m.viewable_text (false, false);
    if (static_cast<int>(bp.size()) > MAX_PREVIEW_LEN)
      bp = bp.substr(0, MAX_PREVIEW_LEN - 3) + "...";

    while (true) {
      size_t i = bp.find ("<br>");

      if (i == ustring::npos) break;

      bp.erase (i, 4);
    }

    bp = Glib::Markup::escape_text (bp);

    webkit_dom_element_set_inner_html (WEBKIT_DOM_ELEMENT(preview), bp.c_str(), (err = NULL, &err));
    */
  }

  g_object_unref (preview);
  g_object_unref (span_body);
  g_object_unref (table_header);
} //

void AstroidExtension::handle_hidden (AstroidMessages::Hidden &msg) {
  /* hide message */
  /*
  WebKitDOMDOMTokenList * class_list =
    webkit_dom_element_get_class_list (WEBKIT_DOM_ELEMENT(div_message));

  webkit_dom_dom_token_list_add (class_list, "hide",
      (err = NULL, &err));

  g_object_unref (class_list);
  */
}

void AstroidExtension::insert_attachments (AstroidMessages::Message m,
    WebKitDOMHTMLElement * div_message) {

  set_attachment_icon (m, div_message); // TODO: if has attachments
}

/* headers  */
void AstroidExtension::insert_header_date (ustring & header, AstroidMessages::Message m)
{
  /* ustring value = ustring::compose ( */
  /*             "<span class=\"hidden_only\">%1</span>" */
  /*             "<span class=\"not_hidden_only\">%2</span>", */
  /*             m.pretty_date (), */
  /*             m.pretty_verbose_date (true)); */

  /* header += create_header_row ("Date", value, true, false); */
}

void AstroidExtension::insert_header_address (
    ustring &header,
    ustring title,
    AstroidMessages::Address address,
    bool important) {

  /* AddressList al (address); */

  /* insert_header_address_list (header, title, al, important); */
}

void AstroidExtension::insert_header_address_list (
    ustring &header,
    ustring title,
    AstroidMessages::AddressList addresses,
    bool important) {

  ustring value;
  bool first = true;

/*   for (Address &address : addresses.addresses) { */
/*     if (address.full_address().size() > 0) { */
/*       if (!first) { */
/*         value += ", "; */
/*       } else { */
/*         first = false; */
/*       } */

/*       value += */
/*         ustring::compose ("<a href=\"mailto:%3\">%4%1%5 &lt;%2&gt;</a>", */
/*           Glib::Markup::escape_text (address.fail_safe_name ()), */
/*           Glib::Markup::escape_text (address.email ()), */
/*           Glib::Markup::escape_text (address.full_address()), */
/*           (important ? "<b>" : ""), */
/*           (important ? "</b>" : "") */
/*           ); */
/*     } */
/*   } */

  header += create_header_row (title, value, important, false, false);
}

void AstroidExtension::insert_header_row (
    ustring &header,
    ustring title,
    ustring value,
    bool important) {

  header += create_header_row (title, value, important, true, false);
}


ustring AstroidExtension::create_header_row (
    ustring title,
    ustring value,
    bool important,
    bool escape,
    bool noprint) {

  return ustring::compose (
      "<div class=\"field %1 %2\" id=\"%3\">"
      "  <div class=\"title\">%3:</div>"
      "  <div class=\"value\">%4</div>"
      "</div>",
      (important ? "important" : ""),
      (noprint ? "noprint" : ""),
      Glib::Markup::escape_text (title),
      header_row_value (value, escape)
      );
}

ustring AstroidExtension::header_row_value (ustring value, bool escape)  {
  return (escape ? Glib::Markup::escape_text (value) : value);
}

void AstroidExtension::set_warning (AstroidMessages::Message m, ustring w) {

}

void AstroidExtension::set_error (AstroidMessages::Message m, ustring w) {

}

/* headers end  */


void AstroidExtension::page_created (WebKitWebExtension * /* extension */,
    WebKitWebPage * _page,
    gpointer /* user_data */) {

  page = _page;
  load_page ();

  std::cout << "ae: page created" << std::endl;

  g_print ("Page %d created for %s\n",
           (int) webkit_web_page_get_id (page),
           webkit_web_page_get_uri (page));
}

