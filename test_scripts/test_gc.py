import unittest
import struct
import inspect
import gmpy2
import sys
import operator
import math
import os
import subprocess
from pathlib import Path
from optparse import OptionParser


input_data_dir = "Input_Data/"

class Preprocessing:
	cwd = os.getcwd()
	root = cwd
	def compile(self, program_name, compile_options):
		#os.system("python compile.py a " + program_name)
		status_code = subprocess.call("python compile.py b {0} {1}".format(compile_options, program_name), shell=True)
		if status_code != 0:
			assert(False)

	def run_online(self, program_name):
		p1 = subprocess.Popen(["./bin/run_circuit 1 5000 {0} {1} {2}".format(program_name, input_data_dir, program_name)], shell=True)
		p2 = subprocess.Popen(["./bin/run_circuit 2 5000 {0} {1} {2}".format(program_name, input_data_dir, program_name)], shell=True)
		exit_codes = [p.wait() for p in p1, p2]
		# Exit code of non-zero means failure.
		if exit_codes[0] != 0 or exit_codes[1] != 0:
			assert(False)

	# Need for compiler to have function write in line.
	def run_test(self, program_name):
		# Read output file 
		output_file = os.path.join(program_name, "agmpc.output")
		outputs = []
		with open(output_file, "r") as f:
			# Length is 4 bytes 
			len_file = struct.unpack('I', f.read(4))[0]
			print "LEN OUTPUT FILE: ", len_file
			# Assume length is multiple of 8.
			bits = f.read()

			
		for i in range(len_file):
			d = bits[i / 8]
			d = ord(d)
			v = int(d & (1 << (i % 8)) > 0)
			print v
			if v != 1:
				assert(False), "Test number {} failed in {}".format(i + 1, program_name)
			
		assert(True)


	def gen_data(self, program_name):
		program_name = program_name.rsplit('/', 1)[-1]
		status_code = subprocess.call("cd Input_Data && python gen_data.py ./ {0}".format(program_name), shell=True)
		if status_code != 0:
			assert(False)


	def check_binary(self, byte, num_ones):
		for i in range(num_ones):
			if (byte >> i) & 1 != 1:
				return False

		return True





class TestGC(unittest.TestCase):
	preprocessing = Preprocessing()
	
	def test_cond(self):
		test_name = 'test_cond'
		program_name = 'Programs/%s' % (test_name)
		self.preprocessing.compile(program_name, '')
		self.preprocessing.gen_data(test_name)
		self.preprocessing.run_online(program_name)
		self.preprocessing.run_test(program_name)
	

	
	
	def test_unroll(self):
		test_name = 'test_unroll'
		program_name = 'Programs/%s' % (test_name)
		self.preprocessing.compile(program_name, "-ur")
		self.preprocessing.gen_data(test_name)
		self.preprocessing.run_online(program_name)
		self.preprocessing.run_test(program_name)

	
	

	def test_inline(self):
		test_name = 'test_inline'
		program_name = 'Programs/%s' % (test_name)
		self.preprocessing.compile(program_name, "-in")
		self.preprocessing.gen_data(test_name)
		self.preprocessing.run_online(program_name)
		self.preprocessing.run_test(program_name)

	
	def test_fused(self):
		test_name = 'test_fused'
		program_name = 'Programs/%s' % (test_name)
		self.preprocessing.compile(program_name, "")
		self.preprocessing.gen_data(test_name)
		self.preprocessing.run_online(program_name)
		self.preprocessing.run_test(program_name)
	

	



if __name__=="__main__":
	unittest.main()



