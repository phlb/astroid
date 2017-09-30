/* --------
 * Public API
 * --------
 */

import * as R from 'ramda/index-es'
import * as U from 'karet.util'
import * as ui from './ui'

/*
const cleanMimeContent = (mimecontent) => {
  const cleanedMimeContent = R.clone(mimecontent)
  if (!Array.isArray(mimecontent.children)) {
    cleanedMimeContent.children = undefined
  }
  return cleanedMimeContent
}
*/

const forceArrayOrNil = R.when(R.complement(Array.isArray), () => undefined)

const toLens = (path) => Array.isArray(path) ? R.lensPath(path) : R.lensProp(path)

/**
 * Clean up invalid/improper JSON that the C++ layer sends us.
 *
 * Astroid's C++ layer is using boost property tree to create json, however this isn't a true
 * json library. It is not capable of serializing empty JSON arrays. Instead what should be empty
 * JSON arrays are sent as empty strings.
 *
 * This function will deeply traverse a Message object and replace all fields that _should_ be
 * empty or undefined Arrays with undefined.
 *
 * @param {Message} message
 * @return {Message} cleaned with all array fields guaranteed to be arrays
 */
const cleanMessage = (message) => {
  const lensToForceAsArray = ['to', 'cc', 'bcc', 'from', 'body'].map(toLens)

  const forceArrayOrNilReducer = (cleanedMessage, lens) => R.over(lens, forceArrayOrNil, cleanedMessage)

  return R.reduce(forceArrayOrNilReducer, message, lensToForceAsArray)
  // cleanedMessage.body = cleanedMessage.body.map(cleanMimeContent)
  // return cleanedMessage
}

/* --------
 * Logging helpers
 * --------
 */

const log = {
  info() {
    // console.log(`[Astroid::${R.head(arguments)}]`, ...R.tail(arguments))
  }
}

/**
 * Initialize the Astroid System and UI
 */
export function init(context) {
  ui.render(context)

  return {
    add_message: R.curry(add_message)(context),
    clear_messages: R.curry(clear_messages)(context)
  }
}

/**
 * Add a message. If it already exists, it will be updated.
 * @param {Message} message - the email message to add
 * @return {void}
 */
function add_message(context, message) {
  log.info('add_message', 'received', message)
  const cleanedMessage = cleanMessage(message)
  log.info('add_message', 'cleaned', cleanedMessage)

  U.view('messages', context.state).modify(U.append(cleanedMessage))
}

/**
 * Clears all messages
 * @return {void}
 */
function clear_messages(context) {
  log.info('clear_messages')
  U.view('messages', context.state).set([])
}


