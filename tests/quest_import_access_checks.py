"""Compile and exercise the production receiver using minimal Android API doubles."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'JDK required')
class ImportAccessChecks(unittest.TestCase):
    def test_dataset_paths_and_traversal(self):
        with tempfile.TemporaryDirectory(prefix='gt2 receiver ') as temp:
            root=Path(temp)
            sources={
                'android/content/BroadcastReceiver.java': 'package android.content; public abstract class BroadcastReceiver { public int code; public void setResultCode(int n){code=n;} public void setResultData(String s){} public abstract void onReceive(Context c, Intent i); }',
                'android/content/Context.java': 'package android.content; import java.io.File; public class Context { private final File root; public Context(File f){root=f;} public File getExternalFilesDir(String s){return root;} }',
                'android/content/Intent.java': 'package android.content; public class Intent { private final String action,paths; public Intent(String a,String p){action=a;paths=p;} public String getAction(){return action;} public String getStringExtra(String s){return paths;} }',
                'Check.java': '''import java.io.*;
import java.nio.file.*;
import android.content.*;
import io.github.gt2pc.quest.ImportAccessReceiver;
public class Check {
 static void check(Context context,String path,boolean verify,int expected) {
  ImportAccessReceiver receiver=new ImportAccessReceiver();
  receiver.onReceive(context,new Intent("io.github.gt2pc.quest."+(verify?"VERIFY_DATA":"PREPARE_DATA"),path));
  if(receiver.code!=expected)throw new AssertionError(path+" result="+receiver.code);
 }
 public static void main(String[] args)throws Exception {
  File root=new File(args[0]); root.mkdirs(); Context context=new Context(root);
  for(String folder:new String[]{"arcade","simulation","combined","combined-plus","simulation-aspec","arcade-mod-ab12","modelsets/gt4"}) {
   check(context,folder+"/assets",false,0);
   Files.write(new File(root,folder+"/assets/.carcolor").toPath(),new byte[]{1});
   check(context,folder+"/assets/.carcolor",true,0);
  }
  for(String path:new String[]{"../escape","combined-plus/../escape","combined-plus/../../escape","/combined-plus","combined-","saves/card1.mcd","combined-plus/assets/missing"})
   check(context,path,true,1);
  check(context,"combined-plus/../escape",false,1);
  System.out.println("Quest receiver dataset and path checks passed");
 }
}'''
            }
            for name,source in sources.items():
                file=root/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_text(source)
            production=ROOT/'android/app/src/main/java/io/github/gt2pc/quest/ImportAccessReceiver.java'
            compile=subprocess.run(['javac','-d',str(root),*[str(root/name) for name in sources],str(production)],capture_output=True,text=True)
            self.assertEqual(compile.returncode,0,compile.stderr)
            run=subprocess.run(['java','-cp',str(root),'Check',str(root/'data')],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)

if __name__ == '__main__':
    unittest.main()
