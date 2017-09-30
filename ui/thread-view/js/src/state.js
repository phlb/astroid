/* --------
 * State management
 * --------
 */

import * as U from 'karet.util'

const meta = {
  version: '0.1'
}

export const newState = (initial_state) => U.atom(initial_state.value)

export function context(state) {
  return {
    meta,
    state
  }
}

