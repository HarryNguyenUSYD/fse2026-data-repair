#!/usr/bin/env python3
"""Generate deterministic repair cases for the FSE benchmark."""
from __future__ import annotations
import json, random, re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parent
CONFIG_PATH, TEST_CASES = ROOT / "suite-config.json", ROOT / "test-cases"
Validator = Callable[[str], bool]
Generator = Callable[[random.Random, int, int], str]

@dataclass(frozen=True)
class Format:
    name: str; category: str; description: str; generator: Generator; validator: Validator

def regex_validator(pattern: str) -> Validator:
    compiled = re.compile(pattern)
    return lambda value: compiled.fullmatch(value) is not None

def date(rng: random.Random, low: int, high: int) -> str:
    if not low <= 10 <= high: raise ValueError("Date range must include length 10")
    return f"{rng.randrange(10000):04d}-{rng.randrange(100):02d}-{rng.randrange(100):02d}"

def time_value(rng: random.Random, low: int, high: int) -> str:
    if not low <= 8 <= high: raise ValueError("Time range must include length 8")
    return f"{rng.randrange(100):02d}:{rng.randrange(100):02d}:{rng.randrange(100):02d}"

def isbn(rng: random.Random, low: int, high: int) -> str:
    for _ in range(2000):
        value = "".join(str(rng.randrange(10)) + rng.choice(("", "-", " ")) for _ in range(9)) + rng.choice("0123456789X")
        if low <= len(value) <= high: return value
    raise ValueError("ISBN range must overlap lengths 10..19")

def ipv6(rng: random.Random, low: int, high: int) -> str:
    for _ in range(2000):
        value = ":".join("".join(rng.choice("0123456789abcdef") for _ in range(rng.randint(1, 4))) for _ in range(8))
        if low <= len(value) <= high: return value
    raise ValueError("IPv6 range must overlap lengths 15..39")

def ipv4(rng: random.Random, low: int, high: int) -> str:
    for _ in range(2000):
        value = ".".join(str(rng.randrange(1000)) for _ in range(4))
        if low <= len(value) <= high: return value
    raise ValueError("IPv4 range must overlap lengths 7..15")

def url(rng: random.Random, low: int, high: int) -> str:
    chars = "abcdefghijklmnopqrstuvwxyz0123456789"
    for _ in range(5000):
        value = rng.choice(("http://", "https://")) + rng.choice(("", "www."))
        value += "".join(rng.choice(chars) for _ in range(rng.randint(1, 20))) + "."
        value += "".join(rng.choice(chars) for _ in range(rng.randint(1, 6)))
        if rng.choice((True, False)): value += "/" + "".join(rng.choice(chars) for _ in range(rng.randint(1, 30)))
        if low <= len(value) <= high: return value
    raise ValueError("Cannot generate URL in configured range")

DATE=r"^\d{4}-\d{2}-\d{2}$"; ISBN=r"^(?:\d[- ]?){9}[\dX]$"
TIME=r"^\d{2}:\d{2}:\d{2}$"; IPV4=r"^(\d{1,3}\.){3}\d{1,3}$"
IPV6=r"^(?:[0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}$"
URL=r"^https?:\/\/(www\.)?[-a-zA-Z0-9@:%._\+~#=]{1,256}\.[a-zA-Z0-9()]{1,6}\b([-a-zA-Z0-9()@:%_\+.~#?&//=]*)$"
FORMATS = (
 Format("date","Date",DATE,date,regex_validator(DATE)), Format("time","Time",TIME,time_value,regex_validator(TIME)),
 Format("url","URL",URL,url,regex_validator(URL)), Format("isbn","ISBN",ISBN,isbn,regex_validator(ISBN)),
 Format("ipv4","IPv4",IPV4,ipv4,regex_validator(IPV4)), Format("ipv6","IPv6",IPV6,ipv6,regex_validator(IPV6)),
)

def edit_distance(left: str, right: str) -> int:
    previous=list(range(len(right)+1))
    for row,a in enumerate(left,1):
        current=[row]
        for column,b in enumerate(right,1): current.append(min(current[-1]+1,previous[column]+1,previous[column-1]+(a!=b)))
        previous=current
    return previous[-1]

def unique(fmt: Format,count:int,rng:random.Random,lengths:dict[str,int],excluded:set[str]|None=None)->set[str]:
    values:set[str]=set(); forbidden=excluded or set()
    for _ in range(100000):
        if len(values)>=count: return values
        value=fmt.generator(rng,lengths["min"],lengths["max"])
        if value not in forbidden and fmt.validator(value): values.add(value)
    raise RuntimeError(f"Cannot generate {count} unique {fmt.name} values")

def mutant(source:str,valid:Validator,distance:int,rng:random.Random,forbidden:set[str])->str:
    for _ in range(10000):
        chars=list(source)
        for _ in range(distance):
            op=rng.choice(["insert"]+(["delete","substitute"] if chars else []))
            if op=="insert": chars.insert(rng.randrange(len(chars)+1),"#")
            elif op=="delete": del chars[rng.randrange(len(chars))]
            else: chars[rng.randrange(len(chars))]="#"
        value="".join(chars)
        if value not in forbidden and not valid(value) and edit_distance(source,value)==distance: return value
    raise RuntimeError(f"Cannot generate distance-{distance} mutant for {source!r}")

def make_case(fmt:Format,index:int,corruption_level:int,cfg:dict[str,Any],rng:random.Random)->dict[str,Any]:
    lengths=cfg["string_lengths"][fmt.name]
    positives=unique(fmt,rng.randint(cfg["S_plus_min"],cfg["S_plus_max"]),rng,lengths)
    negatives:set[str]=set(); target_negatives=rng.randint(cfg["S_minus_min"],cfg["S_minus_max"])
    while len(negatives)<target_negatives:
        source=rng.choice(sorted(positives)); negatives.add(mutant(source,fmt.validator,rng.randint(cfg["d_min"],cfg["d_max"]),rng,positives|negatives))
    examples=positives|negatives
    source=next(iter(unique(fmt,1,rng,lengths,examples)))
    corrupt=mutant(source,fmt.validator,corruption_level,rng,examples|{source})
    return {"case_id":f"{fmt.name}-d{corruption_level}-{index:03d}","format":fmt.name,"category":fmt.category,"positive_examples":sorted(positives),"negative_examples":sorted(negatives),"regex":fmt.description,"corrupt_string":corrupt,"valid_source":source,"true_edit_distance":edit_distance(corrupt,source)}

def generate_cases(cfg:dict[str,Any])->list[dict[str,Any]]:
    if cfg["N"]%len(FORMATS): raise ValueError("N must be divisible by six formats")
    missing={x.name for x in FORMATS}-set(cfg.get("string_lengths",{}))
    if missing: raise ValueError(f"Missing string_lengths: {sorted(missing)}")
    rng=random.Random(cfg["seed"])
    per_format=cfg["N"]//len(FORMATS)
    return [make_case(fmt,i,level,cfg,rng) for level in range(cfg["d_min"],cfg["d_max"]+1) for fmt in FORMATS for i in range(per_format)]

def validate_cases(cases:list[dict[str,Any]],cfg:dict[str,Any])->None:
    formats={x.name:x for x in FORMATS}
    levels=range(cfg["d_min"],cfg["d_max"]+1)
    if len(cases)!=cfg["N"]*len(levels): raise AssertionError("Wrong case count")
    expected=cfg["N"]//len(FORMATS)
    for level in levels:
        for fmt in FORMATS:
            actual=sum(case["format"]==fmt.name and case["true_edit_distance"]==level for case in cases)
            if actual!=expected: raise AssertionError(f"Wrong {fmt.name} distance-{level} count")
    for case in cases:
        fmt=formats[case["format"]]; examples=set(case["positive_examples"])|set(case["negative_examples"])
        if not fmt.validator(case["valid_source"]) or fmt.validator(case["corrupt_string"]): raise AssertionError(f"Validity failure: {case['case_id']}")
        if case["valid_source"] in examples: raise AssertionError(f"Duplicate held-out source: {case['case_id']}")
        if case["corrupt_string"] in examples: raise AssertionError(f"Duplicate corrupt string: {case['case_id']}")
        limits=cfg["string_lengths"][fmt.name]
        if not limits["min"]<=len(case["valid_source"])<=limits["max"]: raise AssertionError(f"Length failure: {case['case_id']}")
        measured=edit_distance(case["corrupt_string"],case["valid_source"])
        if measured!=case["true_edit_distance"] or not cfg["d_min"]<=measured<=cfg["d_max"]: raise AssertionError(f"Distance failure: {case['case_id']}")

def main()->None:
    cfg=json.loads(CONFIG_PATH.read_text(encoding="utf-8")); cases=generate_cases(cfg); validate_cases(cases,cfg)
    TEST_CASES.mkdir(parents=True,exist_ok=True); path=TEST_CASES/"test-cases.json"
    path.write_text(json.dumps(cases,indent=2)+"\n",encoding="utf-8"); print(f"generated {path}: {len(cases)} cases")
if __name__=="__main__": main()
