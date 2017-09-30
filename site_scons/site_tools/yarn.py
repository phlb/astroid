import SCons.Builder
import os
import shutil
from subprocess import Popen

def yarnAction (target, source, env):
  '''
  run yarn 
  '''

  # set up target directory
  d = os.path.dirname (target[0].rstr())
  if not os.path.exists (d):
    os.makedirs (d)

  cwd = os.path.dirname (source[0].rstr())

  process = Popen ("yarn -s --no-progress --non-interactive", cwd = cwd, shell = True)

  process.wait ()
  ret = process.returncode

  return ret

def yarnActionString(target, source, env):
  '''
  Return output string
  '''
  return 'yarn: bundling js library: ' + str(target[0])

def generate (env):
  env['BUILDERS']['Yarn'] = env.Builder(
      action = env.Action(yarnAction, yarnActionString),
      suffix='.js')

def addYarn (env, target=None, *args, **kwargs):
  '''
  Parameters:
         target - the target dist file
         source - directory where yarn should be run 
         any additional parameters are passed along directly to env.Program().
  Returns:
         The scons node for the unit test.
  '''

  y = env.Yarn (target)

  # make an alias to run the test in isolation from the rest of the tests.
  env.Alias(str(target), y)

  return y 

from SCons.Script.SConscript import SConsEnvironment
SConsEnvironment.addYarn = addYarn

def exists (env):
  return true

