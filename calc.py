#!/usr/bin/env python

import sys
import math
from types import *
from pprint import pprint


Tplain, Tpow2range, Tpow10range, Tinterval = range(4)
keys = {
	"maxdatafile":		Tpow2range,
	"blocksize":		Tpow2range,
	"collisioninterval":	Tpow10range,

	"maxinitmem":		Tplain,
	"maxtotalmem":		Tplain,
	"minchainentries":	Tpow2range,
	"minmaxblocksperhead":	Tinterval,
}

def log2(n):
	return math.log(n)/math.log(2)

def fromintsuffix(s):
	suffix = ['', 'k', 'm', 'g', 't']
	num = ""
	while s and s[0:1].isdigit():
		num += s[0:1]
		s = s[1:]
	i = int(num)
	if not s in suffix:
		raise Exception("invalid integer suffix")
	i = int(i * math.pow(1024, suffix.index(s)))
	return i

def toroundsuffix(num):
	suffix = ['', 'k', 'm', 'g', 't']
	i = 0
	while num >= 1024*10:
		num /= 1024
		i += 1
	return str(num)+suffix[i]


def mkrange(rangestr, f):
	val = []
	for startendstr in rangestr.split(","):
		if not "-" in startendstr:
			val.append(fromintsuffix(startendstr))
		else:
			startstr, endstr = startendstr.split("-")
			start = fromintsuffix(startstr)
			end = fromintsuffix(endstr)
			while start <= end:
				val.append(start)
				start = f(start)
	return val

def mkpow2range(startendstr):
	return mkrange(startendstr, lambda x: x*2)
	
def mkpow10range(startendstr):
	return mkrange(startendstr, lambda x: x*10)

def mkinterval(startendstr):
	if startendstr.startswith("-"):
		return (0, fromintsuffix(startendstr[1:]))
	startstr, endstr = startendstr.split("-")
	return fromintsuffix(startstr), fromintsuffix(endstr)

def mkcfgs(config):
	cfgs = [{}]
	for key, value in config.items():
		if type(value) == NoneType or type(value) == TupleType or type(value) == IntType:
			for c in cfgs:
				c[key] = value
		elif type(value) == ListType:
			newcfgs = []
			for elem in value:
				for c in cfgs:
					c = c.copy()
					c[key] = elem
					newcfgs.append(c)
			cfgs = newcfgs
		else:
			raise Exception("invalid type of value: %r" % type(value))
	return cfgs


def warn(s):
	print >>sys.stderr, s

def usage(prog):
	usagestr = "usage: %s maxdatafile start-end blocksize start-end collisioninterval start-end" + \
		" [maxinitmem size] [maxtotalmem size] [minchainentries start-end] [minmaxblocksperhead start-end]"
	print >>sys.stderr, usagestr % prog
	sys.exit(1)


def main(prog, *args):
	required = ["maxdatafile", "blocksize", "collisioninterval"]
	config = {}

	if len(args) % 2 != 0:
		usage(prog)
	while len(args) != 0:
		key, value, args = args[0], args[1], args[2:]
		if key not in keys:
			usage(prog)
		ktype = keys[key]
		if ktype == Tplain:
			value = fromintsuffix(value)
		elif ktype == Tpow2range:
			value = mkpow2range(value)
		elif ktype == Tpow10range:
			value = mkpow10range(value)
		elif ktype == Tinterval:
			value = mkinterval(value)
		else:
			raise Exception("internal error")

		config[key] = value

	missing = list(set(required) - set(config.keys()))
	if len(missing) != 0:
		warn("missing keys: %s" % ", ".join(missing))
		usage(prog)

	if not "minchainentries" in config:
		config["minchainentries"] = [4, 8]

	for k in keys.keys():
		if not k in config:
			config[k] = None

	cfgs = mkcfgs(config)

	chainsize = 9
	results = []
	for cfg in cfgs:
		maxdatafile =		cfg["maxdatafile"]
		blocksize =		cfg["blocksize"]
		collisioninterval =	cfg["collisioninterval"]
		maxinitmem =		cfg["maxinitmem"]
		maxtotalmem =		cfg["maxtotalmem"]
		minchainentries =	cfg["minchainentries"]
		minmaxblocksperhead =	cfg["minmaxblocksperhead"]
		
		totalblocks = maxdatafile / blocksize
		addrwidth = int(0.9 + round(log2(maxdatafile), 1))
		totalscorewidth = int(0.9 + log2(totalblocks)) + int(0.9 + round(abs(log2(1.0/collisioninterval)), 1))

		result = {
		"config":		cfg,
		"totalblocks":		toroundsuffix(totalblocks),
		"addrwidth":		addrwidth,
		"totalscorewidth":	totalscorewidth,
		}

		for i in range(1, totalscorewidth+1):
			headscorewidth = i
			entryscorewidth = totalscorewidth - headscorewidth
			if headscorewidth <= 0 or entryscorewidth <= 0:
				continue
			nheads = 2**headscorewidth
			minheadmem = nheads*chainsize
			entrywidth = entryscorewidth+addrwidth+8
			chaindatabytes = (entrywidth*minchainentries+ 8 - 1) / 8
			headunused = int(1.0+(chainsize+chaindatabytes)/2.0)
			blocksperhead = int(1.0 + float(totalblocks)/nheads)
			chainsperhead = int(1.0 + float(blocksperhead)/minchainentries)
			maxmemused = int(minheadmem + nheads*(0.5+chainsperhead)*(chainsize+chaindatabytes))
			memperblock = float(maxmemused) / totalblocks

			n = totalblocks
			d = 2**totalscorewidth
			collisions = int(round(n - d + d*math.pow((float(d)-1)/float(d), n)))
			# xxx also calculate 2nd order and further collisions

			if maxinitmem != None and minheadmem > maxinitmem:
				continue
			if maxtotalmem != None and maxmemused > maxtotalmem:
				continue
			if minmaxblocksperhead != None and (blocksperhead < minmaxblocksperhead[0] or blocksperhead > minmaxblocksperhead[1]):
				continue

			newresult = result.copy()
			newresult.update({
			"headscorewidth":	headscorewidth,
			"entryscorewidth":	entryscorewidth,
			"headcount":		toroundsuffix(nheads),
			"blocksperhead":	toroundsuffix(blocksperhead),
			"entrywidth":		entrywidth,
			"chaindatasize":	toroundsuffix(chaindatabytes),

			"minmemused":		toroundsuffix(minheadmem),
			"maxmemused":		toroundsuffix(maxmemused),
			"unusedmem":		toroundsuffix(nheads*headunused),
			"collisions":		collisions,
			"memperblock":		memperblock,
			"sort":			maxmemused,
			})
			results.append(newresult)

	print "results:"
	results.sort(lambda x, y: cmp(x['sort'], y['sort']))
	pprint(results)
		

if __name__ == "__main__":
	main(*sys.argv)
