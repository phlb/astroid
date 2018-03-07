# include <string>
# include <vector>
# include <gio/gio.h>
# include <iostream>

#ifdef ASTROID_WEBEXTENSION
# include <webkit2/webkit-web-extension.h>
# else
# include <webkit2/webkit2.h>
# endif

# include "dom_utils.hh"

using std::cout;
using std::endl;
using std::vector;

namespace Astroid {
  std::string DomUtils::assemble_data_uri (ustring mime_type, gchar * &data, gsize len) {

    std::string base64 = "data:" + mime_type + ";base64," + Glib::Base64::encode (std::string(data, len));

    return base64;
  }

# ifdef ASTROID_WEBEXTENSION

  /* clone and create html elements */
  WebKitDOMHTMLElement * DomUtils::make_message_div (WebKitDOMDocument * d) {
    /* clone div from template in html file */
    WebKitDOMHTMLElement * e = clone_node (WEBKIT_DOM_NODE(get_by_id (d, "email_template")));
    return e;
  }

  WebKitDOMHTMLElement * DomUtils::clone_get_by_id (
      WebKitDOMDocument * node,
      ustring         id,
      bool            deep) {

    return clone_node (WEBKIT_DOM_NODE (get_by_id (node, id)), deep);
  }

  WebKitDOMHTMLElement * DomUtils::clone_select (
      WebKitDOMNode * node,
      ustring         selector,
      bool            deep) {

    return clone_node (WEBKIT_DOM_NODE(select (node, selector)), deep);
  }

  WebKitDOMHTMLElement * DomUtils::clone_node (
      WebKitDOMNode * node,
      bool            deep) {

    GError * gerr = NULL;
    return WEBKIT_DOM_HTML_ELEMENT(webkit_dom_node_clone_node_with_error (node, deep, &gerr));
  }

  WebKitDOMHTMLElement * DomUtils::select (
      WebKitDOMNode * node,
      ustring         selector) {

    GError * gerr = NULL;
    WebKitDOMHTMLElement *e;

    if (WEBKIT_DOM_IS_DOCUMENT(node)) {
      e = WEBKIT_DOM_HTML_ELEMENT(
        webkit_dom_document_query_selector (WEBKIT_DOM_DOCUMENT(node),
                                            selector.c_str(),
                                            &gerr));
    } else {
      /* ..DOMElement */
      e = WEBKIT_DOM_HTML_ELEMENT(
        webkit_dom_element_query_selector (WEBKIT_DOM_ELEMENT(node),
                                            selector.c_str(),
                                            &gerr));
    }

    if (gerr != NULL)
      std::cout << "dom: error: " << gerr->message << std::endl;

    return e;
  }

  WebKitDOMElement * DomUtils::get_by_id (WebKitDOMDocument * d, ustring id) {
    WebKitDOMElement * en = webkit_dom_document_get_element_by_id (d, id.c_str());

    return en;
  }

  bool DomUtils::switch_class (WebKitDOMDOMTokenList * list, ustring c, bool v)
  {
    GError * err = NULL;

    bool x = webkit_dom_dom_token_list_contains (list, c.c_str ());

    if (v && !x) {
      webkit_dom_dom_token_list_add (list, &err, c.c_str (), NULL );
    } else if (!v && x) {
      webkit_dom_dom_token_list_remove (list, &err, c.c_str (), NULL );
    }

    return x;
  }
# endif

}

