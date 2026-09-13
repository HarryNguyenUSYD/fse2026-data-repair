"""Real-process protocol tests; build all and oracle_driver before running."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import runner_support as runner

BUILD = Path(os.environ.get("ORACLE_TEST_BUILD", runner.BUILD)).resolve()
SUFFIX = runner.EXE_SUFFIX
VALID = {"date": "2026-09-12", "time": "12:34:56", "isbn": "123456789X",
         "ipv4": "192.168.0.1", "ipv6": "1:2:3:4:5:6:7:8", "url": "https://example.com"}

class OracleIntegrationTests(unittest.TestCase):
    def run_process(self, executable, payload, *args):
        return subprocess.run([str(executable), *map(str,args)], input=payload,
                              text=True, capture_output=True, timeout=30)

    def validator(self, name):
        return BUILD / ("validate_" + name + SUFFIX)

    def test_all_validator_protocols(self):
        for name, valid in VALID.items():
            for values in ([], [valid], ["invalid"], [valid, "invalid", valid, "", '\n"\\\x00']):
                with self.subTest(name=name, values=values):
                    p=self.run_process(self.validator(name),json.dumps(values))
                    self.assertEqual(p.returncode,0,p.stderr)
                    self.assertEqual(json.loads(p.stdout),[v==valid for v in values])
            for malformed in ('', 'raw', '{}', '"string"', '[1]', '[true]', '[null]', '["ok", {}]'):
                with self.subTest(name=name, malformed=malformed):
                    p=self.run_process(self.validator(name),malformed)
                    self.assertNotEqual(p.returncode,0)
                    self.assertEqual(p.stdout,"")

    def test_external_oracle_and_large_pipes(self):
        driver=BUILD / ("oracle_driver"+SUFFIX)
        for name, valid in VALID.items():
            values=[valid,"invalid",valid,"\n",""]
            p=self.run_process(driver,json.dumps(values),self.validator(name))
            self.assertEqual(p.returncode,0,p.stderr)
            self.assertEqual(json.loads(p.stdout),[True,False,True,False,False])
        values=[VALID["date"],"invalid"]*20000
        p=self.run_process(driver,json.dumps(values),self.validator("date"))
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(json.loads(p.stdout),[True,False]*20000)
        p=self.run_process(driver,'[]',BUILD/'missing-executable')
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(json.loads(p.stdout),[])
        p=self.run_process(driver,'["x"]',BUILD/'missing-executable')
        self.assertNotEqual(p.returncode,0)

    def test_external_oracle_protocol_errors(self):
        driver=BUILD / ("oracle_driver"+SUFFIX)
        with tempfile.TemporaryDirectory() as directory:
            for mode in ('bad_json','wrong_count','wrong_type','nonarray','failure'):
                fixture=Path(directory)/('fixture_'+mode+SUFFIX)
                shutil.copy2(driver,fixture)
                p=self.run_process(driver,'["x"]',fixture)
                self.assertNotEqual(p.returncode,0,(mode,p.stdout))

    def test_harness_singleton_validation(self):
        validators={name:self.validator(name) for name in VALID}
        with patch.object(runner,'STDIN_VALIDATORS',validators):
            for name, valid in VALID.items():
                self.assertTrue(runner._validator_accepts(name,valid))
                self.assertFalse(runner._validator_accepts(name,'invalid'))
        for output in ('true','[]','[1]','[true,false]','bad'):
            result=subprocess.CompletedProcess([],0,output,'')
            with patch.object(runner.subprocess,'run',return_value=result):
                with self.assertRaises(ValueError): runner._validator_accepts('date','x')

if __name__=='__main__': unittest.main()
