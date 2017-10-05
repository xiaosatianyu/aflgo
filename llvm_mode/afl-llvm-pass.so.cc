/*
 american fuzzy lop - LLVM-mode instrumentation pass
 ---------------------------------------------------

 Written by Laszlo Szekeres <lszekeres@google.com> and
 Michal Zalewski <lcamtuf@google.com>

 LLVM integration design comes from Laszlo Szekeres. C bits copied-and-pasted
 from afl-as.c are Michal's fault.

 Copyright 2015, 2016 Google Inc. All rights reserved.

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at:

 http://www.apache.org/licenses/LICENSE-2.0

 This library is plugged into LLVM when invoking clang through afl-clang-fast.
 It tells the compiler to add code roughly equivalent to the bits discussed
 in ../afl-as.h.

 */

#define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <list>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/CFGPrinter.h"

#if defined(LLVM34)
#include "llvm/DebugInfo.h"
#else
#include "llvm/IR/DebugInfo.h"
#endif

#if defined(LLVM34) || defined(LLVM35) || defined(LLVM36)
#define LLVM_OLD_DEBUG_API
#endif

using namespace llvm;
//这种参数的方式,klee中也有  cl是一个namespace,解析参数
cl::opt<std::string> DistanceFile("distance",
		cl::desc(
				"Distance file containing the distance of each basic block to the provided targets."),
		cl::value_desc("filename"));

cl::opt<std::string> TargetsFile("targets",
		cl::desc("Input file containing the target lines of code."),
		cl::value_desc("targets"));

cl::opt<std::string> OutDirectory("outdir",
		cl::desc(
				"Output directory where Ftargets.txt, Fnames.txt, and BBnames.txt are generated."),
		cl::value_desc("outdir"));

//
namespace {

class AFLCoverage: public ModulePass {

public:

	static char ID;
	AFLCoverage() :
			ModulePass(ID) {
	}

	bool runOnModule(Module &M) override;  //ModulePass下要重写的方法

};

}

char AFLCoverage::ID = 0;
//重写runOnModule方法  什么时候会进入这个函数?
bool AFLCoverage::runOnModule(Module &M) {  //这里是将整个系统都当做一个类了吧
	//for debug
//	std::ofstream yytest;
//	yytest.open(OutDirectory + "/yytest.txt",std::ofstream::out | std::ofstream::app);

	bool is_aflgo = false; //true表示距离编译
	bool is_aflgo_preprocessing = false;
	// -target的时候表示为了计算距离   -distance的时候表示进行针对性的插桩 // 不能同时有 target和distance
	if (!TargetsFile.empty() && !DistanceFile.empty()) {
		FATAL("Cannot specify both '-targets' and '-distance'!");
		return false;
	}

	std::list < std::string > targets;  //记录所有目标
	std::map<std::string, int> bb_to_dis; //记录对应行和目标距离的一个map
	std::vector < std::string > basic_blocks; //记录所有的行

	if (!TargetsFile.empty()) {

		if (OutDirectory.empty()) {
			FATAL("Provide output directory '-outdir <directory>'");
			return false;
		}
		//add by xiaosa-------
		// 创建output目录
		struct stat sby;
		std::string youtput(OutDirectory);
		if (stat(youtput.c_str(), &sby) != 0) {
			const int dir_err = mkdir(youtput.c_str(),
					S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
			if (-1 == dir_err)
				FATAL("Could not create directory %s.", youtput.c_str());
		}
		//-------


		//读取所有的targets
		std::ifstream targetsfile(TargetsFile);
		std::string line;
		while (std::getline(targetsfile, line))
			targets.push_back(line);
		targetsfile.close();

		//aflgo预处理
		is_aflgo_preprocessing = true; //读到目标地址就要预处理了, 表示准备计算距离
		SAYF(cCYA "read target sucess!\n");
	} else if (!DistanceFile.empty()) {  // distance.cfg.txt文件 某一行和目标之间的距离

		std::ifstream cf(DistanceFile.c_str()); //c_str() 函数表示 返回一个指向正规C字符串的指针常量
		if (cf.is_open()) {

			std::string line;
			while (getline(cf, line)) {

				std::size_t pos = line.find(",");  //前一个表示行或者函数,后面一个表示和目标的距离
				std::string bb_name = line.substr(0, pos);  // bb_name 表示行号
				int bb_dis = (int) (100.0
						* atof(line.substr(pos + 1, line.length()).c_str())); //得到该行和目标之间的距离
				bb_to_dis.insert(std::pair<std::string, int>(bb_name, bb_dis));
				basic_blocks.push_back(bb_name); //记录所有的bb_name ,这里的是一个行

			}
			cf.close();

			is_aflgo = true;

		} else {
			FATAL("Unable to find %s.", DistanceFile.c_str());
			return false;
		}

	}

	LLVMContext &C = M.getContext(); //得到上下文信息吧

	IntegerType *Int8Ty = IntegerType::getInt8Ty(C); //得到类型
	IntegerType *Int32Ty = IntegerType::getInt32Ty(C);
	IntegerType *Int64Ty = IntegerType::getInt64Ty(C);

	/* Show a banner */

	char be_quiet = 0;

	if (isatty(2) && !getenv("AFL_QUIET"))
	{ //isatty(2) 检查屏幕输出
		if (is_aflgo || is_aflgo_preprocessing)
			SAYF( cCYA "aflgo-llvm-pass (yeah!) " cBRI VERSION cRST " (%s mode)\n",
				(is_aflgo_preprocessing ?"preprocessing" : "distance instrumentation"));
		else
			SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <lszekeres@google.com>\n");

	} else
		be_quiet = 1;

	/* Decide instrumentation ratio */

	char* inst_ratio_str = getenv("AFL_INST_RATIO");
	unsigned int inst_ratio = 100;

	if (inst_ratio_str) {

		if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio
				|| inst_ratio > 100)
			FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

	}

	/* Default: Not selecitive */
	char* is_selective_str = getenv("AFLGO_SELECTIVE");
	unsigned int is_selective = 0;

	if (is_selective_str && sscanf(is_selective_str, "%u", &is_selective) != 1)
		FATAL("Bad value of AFLGO_SELECTIVE (must be 0 or 1)");

	char* dinst_ratio_str = getenv("AFLGO_INST_RATIO");
	unsigned int dinst_ratio = 100;

	if (dinst_ratio_str) {

		if (sscanf(dinst_ratio_str, "%u", &dinst_ratio) != 1 || !dinst_ratio
				|| dinst_ratio > 100)
			FATAL("Bad value of AFLGO_INST_RATIO (must be between 1 and 100)");

	}

	/* Get globals for the SHM region and the previous location. Note that
	 __afl_prev_loc is thread-local. */

	GlobalVariable *AFLMapPtr = new GlobalVariable(M,
			PointerType::get(Int8Ty, 0), false, GlobalValue::ExternalLinkage, 0,
			"__afl_area_ptr");  //得到一个指针 得到全局变量,每次执行轨迹

	GlobalVariable *AFLPrevLoc = new GlobalVariable(M, Int32Ty, false,
			GlobalValue::ExternalLinkage, 0, "__afl_prev_loc", 0,
			GlobalVariable::GeneralDynamicTLSModel, 0, false); //创建一个全局变量 得到一个指针  总的执行轨迹

	/* Instrument all the things! */

	int inst_blocks = 0;

	if (is_aflgo_preprocessing) {
		SAYF(cCYA "step in as aflgo_preprocessing!\n");
		std::ofstream bbnames;
		std::ofstream bbcalls;
		std::ofstream fnames;
		std::ofstream ftargets;

		struct stat sb;

		bbnames.open(OutDirectory + "/BBnames.txt",
				std::ofstream::out | std::ofstream::app);
		bbcalls.open(OutDirectory + "/BBcalls.txt",
				std::ofstream::out | std::ofstream::app);
		fnames.open(OutDirectory + "/Fnames.txt",
				std::ofstream::out | std::ofstream::app);
		ftargets.open(OutDirectory + "/Ftargets.txt",
				std::ofstream::out | std::ofstream::app);


		/* Create dot-files directory */
		std::string dotfiles(OutDirectory + "/dot-files");
		if (stat(dotfiles.c_str(), &sb) != 0) {
			const int dir_err = mkdir(dotfiles.c_str(),
					S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
			if (-1 == dir_err)
				FATAL("Could not create directory %s.", dotfiles.c_str());
		}
		//遍历每个函数,得到cfg 这个是for else
		for (auto &F : M) {

			bool has_BBs = false; //表示找到bb
			std::string funcName = F.getName();

			/* Black list of function names */
			std::vector<std::string> blacklist = {
				"asan.",
				"llvm.",
				"sancov.",
				"free"
				"malloc",
				"calloc",
				"realloc"
			};
			for (std::vector<std::string>::size_type i = 0;
					i < blacklist.size(); i++)
				if (!funcName.compare(0, blacklist[i].size(), blacklist[i]))
					continue;

			bool is_target = false;
			//遍历函数下的所有BB
			for (auto &BB : F) {
				TerminatorInst *TI = BB.getTerminator(); //得到最后一条指令
				IRBuilder<> Builder(TI);

				std::string bb_name("");
				std::string filename;
				unsigned line;
				//遍历所有指令
				for (auto &I : BB) {
#ifdef LLVM_OLD_DEBUG_API
					DebugLoc Loc = I.getDebugLoc();
					if (!Loc.isUnknown()) {

						DILocation cDILoc(Loc.getAsMDNode(M.getContext()));
						DILocation oDILoc = cDILoc.getOrigLocation();

						line = oDILoc.getLineNumber();
						filename = oDILoc.getFilename().str();

						if (filename.empty()) {
							line = cDILoc.getLineNumber();
							filename = cDILoc.getFilename().str();
						}
#else

					if (DILocation *Loc = I.getDebugLoc()) {
						line = Loc->getLine(); //得到对应指令在源代码中的行号
						filename = Loc->getFilename().str(); //函数名称

						if (filename.empty()) {
							DILocation *oDILoc = Loc->getInlinedAt();
							if (oDILoc) {
								line = oDILoc->getLine();
								filename = oDILoc->getFilename().str();
							}
						}

#endif /* LLVM_OLD_DEBUG_API */

						/* Don't worry about external libs */
						std::string Xlibs("/usr/");
						if (filename.empty() || line == 0
								|| !filename.compare(0, Xlibs.size(), Xlibs))
							continue;

						if (bb_name.empty()) {

							std::size_t found = filename.find_last_of("/\\");
							if (found != std::string::npos)
								filename = filename.substr(found + 1);

							bb_name = filename + ":" + std::to_string(line);

						}
						//判断当前的指令所在行号所在位置是否是目标,  结果放在 is_target
						if (!is_target) {
							for (std::list<std::string>::iterator it =
									targets.begin(); it != targets.end();
									++it) {
								std::string target = *it;
								std::size_t found = target.find_last_of("/\\");
								if (found != std::string::npos)
									target = target.substr(found + 1);

								std::size_t pos = target.find_last_of(":");
								std::string target_file = target.substr(0, pos);
								unsigned int target_line = atoi(
										target.substr(pos + 1).c_str());
								if (!target_file.compare(filename)
										&& target_line == line)
									{
									is_target = true;}

							}
						}

						//如果当前指令是call指令,即调用别的函数
						if (auto *c = dyn_cast < CallInst > (&I)) {

							std::size_t found = filename.find_last_of("/\\");
							if (found != std::string::npos)
								filename = filename.substr(found + 1);

							if (c->getCalledFunction()) {
								std::string called =
										c->getCalledFunction()->getName().str();

								bool blacklisted = false;
								for (std::vector<std::string>::size_type i = 0;
										i < blacklist.size(); i++) {
									if (!called.compare(0, blacklist[i].size(),
											blacklist[i])) {
										blacklisted = true;
										break;
									}
								}
								if (!blacklisted)
									bbcalls << bb_name << "," << called << "\n"; //表示某一行调用的函数  记录在某一行,与目标的距离,调用某个函数
							}
						}
					}
				}

				if (!bb_name.empty()) {

					BB.setName(bb_name + ":");
					if (!BB.hasName()) {
						std::string newname = bb_name + ":";
						Twine t(newname);
						SmallString < 256 > NameData;
						StringRef NameRef = t.toStringRef(NameData);
						BB.setValueName(ValueName::Create(NameRef));
					}

					bbnames << BB.getName().str() << "\n";
					has_BBs = true;

#ifdef AFLGO_TRACING
					Value *bbnameVal = Builder.CreateGlobalStringPtr(bb_name);
					Type *Args[] = {
						Type::getInt8PtrTy(M.getContext()) //uint8_t* bb_name
					};
					FunctionType *FTy = FunctionType::get(Type::getVoidTy(M.getContext()), Args, false);
					Constant *instrumented = M.getOrInsertFunction("llvm_profiling_call", FTy);
					Builder.CreateCall(instrumented, {bbnameVal});
#endif

				}
			}
			//end  for (auto &BB : F)

			//如果识别基本块的话
			if (has_BBs) {
				/* Print CFG */
				std::string cfgFileName = dotfiles + "/cfg." + funcName
						+ ".dot";
				struct stat buffer;
				if (stat(cfgFileName.c_str(), &buffer) != 0) {
					FILE *cfgFILE = fopen(cfgFileName.c_str(), "w");
					if (cfgFILE) {
						raw_ostream *cfgFile = new llvm::raw_fd_ostream(
								fileno(cfgFILE), false, true);

						WriteGraph(*cfgFile, (const Function*) &F, true); //这函数内部应该会有处理, 比如名称啥的
						fflush(cfgFILE);
						fclose(cfgFILE);
					}
				}

				if (is_target)
					ftargets << F.getName().str() << "\n";
				fnames << F.getName().str() << "\n";
			}
		}
		//end  for (auto &I : BB)

		bbnames.close();
		bbcalls.close();
		fnames.close();
		ftargets.close();

	} else {
		//根据距离,进行编译插桩
		for (auto &F : M) {

			int distance = -1;

			for (auto &BB : F) {

				distance = -1;

				if (is_aflgo) {
					TerminatorInst *TI = BB.getTerminator(); //返回结束指令,或者null
					IRBuilder<> Builder(TI);

					std::string bb_name;
					//这里原始的afl是没有进去的,这里进去的目的是?? 根据行号读取distance, 随便根据一个基本块中的一个行号,得到当前基本块和目标之间的距离
					for (auto &I : BB) {

#ifdef LLVM_OLD_DEBUG_API
						DebugLoc Loc = I.getDebugLoc();
						if (!Loc.isUnknown()) {

							DILocation cDILoc(Loc.getAsMDNode(M.getContext()));
							DILocation oDILoc = cDILoc.getOrigLocation();

							unsigned line = oDILoc.getLineNumber();
							std::string filename = oDILoc.getFilename().str();

							if (filename.empty()) {
								line = cDILoc.getLineNumber();
								filename = cDILoc.getFilename().str();
							}
#else
						if (DILocation *Loc = I.getDebugLoc()) {

							unsigned line = Loc->getLine(); //基本块第一个指令所在的行
							std::string filename = Loc->getFilename().str(); //当前行所在的文件

							if (filename.empty()) {
								DILocation *oDILoc = Loc->getInlinedAt();
								if (oDILoc) {
									line = oDILoc->getLine();
									filename = oDILoc->getFilename().str();
								}
							}
#endif /* LLVM_OLD_DEBUG_API */

							if (filename.empty() || line == 0)
								continue;
							std::size_t found = filename.find_last_of("/\\"); //找到最后'\'
							if (found != std::string::npos) //std::string::npos 表示strig的结束位
								filename = filename.substr(found + 1);

							bb_name = filename + ":" + std::to_string(line); // 基本块第一个指令,得到bb_name 文件名:行号
							break; //得到一个  bb_name 就退出? 即当前基本块的距离 基本块所在第一行的距离 取代基本块的距离

						}

					} // end for (auto &I : BB)

					if (!bb_name.empty()) {

						if (find(basic_blocks.begin(), basic_blocks.end(),
								bb_name) == basic_blocks.end()) {

							if (is_selective)
								continue;

						} else {

							/* Find distance for BB */
							//读取对应行的距离,作为基本块的距离
							if (AFL_R(100) < dinst_ratio) {
								std::map<std::string, int>::iterator it;
								for (it = bb_to_dis.begin();
										it != bb_to_dis.end(); ++it)
									if (it->first.compare(bb_name) == 0)
										distance = it->second;

								/* DEBUG */
								// ACTF("Distance for %s\t: %d", bb_name.c_str(), distance);
							}
						}
					}
				} // end if (is_aflgo), get a  new distance

				BasicBlock::iterator IP = BB.getFirstInsertionPt(); //得到一个iterator,指向第一个指令,可以方便的插入non-PHI指令
				IRBuilder<> IRB(&(*IP)); //根据第一条指令生成一个ir

				if (AFL_R(100) >= inst_ratio)
					continue;
				//这里进行原始afl的插桩,这里只需要在对应的内存上记录就行
				/* Make up cur_loc */

				unsigned int cur_loc = AFL_R(MAP_SIZE); //返回一个随机值  这个怎么会是当前位置呢?

				ConstantInt *CurLoc = ConstantInt::get(Int32Ty, cur_loc);

				/* Load prev_loc */

				LoadInst *PrevLoc = IRB.CreateLoad(AFLPrevLoc); //加载一个指针,利用load进行取值
				PrevLoc->setMetadata(M.getMDKindID("nosanitize"),
						MDNode::get(C, None)); //在PrevLoc这里插桩吗?  Metadata是调试信息 这里应该是增加调试信息
				Value *PrevLocCasted = IRB.CreateZExt(PrevLoc,
						IRB.getInt32Ty()); //插入指令,,第一个是值,第二个是类型 CreateZExt函数的功能,

				/* Load SHM pointer */

				LoadInst *MapPtr = IRB.CreateLoad(AFLMapPtr); //加载一个指针,利用load进行取值
				MapPtr->setMetadata(M.getMDKindID("nosanitize"),
						MDNode::get(C, None));
				Value *MapPtrIdx = IRB.CreateGEP(MapPtr,
						IRB.CreateXor(PrevLocCasted, CurLoc)); //得到map上的值 CreateGEP函数 CreateXor函数表示插入比较的指令,异或算出元组吗,然后指向共享内存上指定的地方

				/* Update bitmap */ //将对应执行的元组关系 记录到共享内存中
				LoadInst *Counter = IRB.CreateLoad(MapPtrIdx); //加载共享内存上的值
				Counter->setMetadata(M.getMDKindID("nosanitize"),
						MDNode::get(C, None));
				Value *Incr = IRB.CreateAdd(Counter,
						ConstantInt::get(Int8Ty, 1)); //插入指令,共享内存上对应位置加1
				IRB.CreateStore(Incr, MapPtrIdx)->setMetadata(
						M.getMDKindID("nosanitize"), MDNode::get(C, None)); //保存共享内存上

				/* Set prev_loc to cur_loc >> 1 */ // 保存到 AFLPrevLoc 中
				StoreInst *Store = IRB.CreateStore(
						ConstantInt::get(Int32Ty, cur_loc >> 1), AFLPrevLoc);
				Store->setMetadata(M.getMDKindID("nosanitize"),
						MDNode::get(C, None));

				//这里是新加的,前面都没有变
				if (distance >= 0) {

					unsigned int udistance = (unsigned) distance;

#ifdef __x86_64__
					IntegerType *LargestType = Int64Ty; //表示8个字节
					ConstantInt *MapDistLoc = ConstantInt::get(LargestType, MAP_SIZE);
					ConstantInt *MapCntLoc = ConstantInt::get(LargestType, MAP_SIZE + 8);//64位的就是这个
					ConstantInt *Distance = ConstantInt::get(LargestType, udistance);
#else
					IntegerType *LargestType = Int32Ty; //表示4个字节
					ConstantInt *MapDistLoc = ConstantInt::get(LargestType,
					MAP_SIZE); //从这里开始 记录一个数据,记录距离总和
					ConstantInt *MapCntLoc = ConstantInt::get(LargestType,
					MAP_SIZE + 4); //从这里开始也记录一个数据,记录数量总和
					ConstantInt *Distance = ConstantInt::get(LargestType,
							udistance);
#endif

					/* Add distance to shm[MAPSIZE] */

					Value *MapDistPtr = IRB.CreateGEP(MapPtr, MapDistLoc); // 指向对应的位置
#ifdef LLVM_OLD_DEBUG_API
							LoadInst *MapDist = IRB.CreateLoad(MapDistPtr);
							MapDist->mutateType(LargestType);
#else
					LoadInst *MapDist = IRB.CreateLoad(LargestType, MapDistPtr); //读取MapDistPtr位置的数据
#endif
					//update bitmap according the distance
					MapDist->setMetadata(M.getMDKindID("nosanitize"),
							MDNode::get(C, None));
					Value *IncrDist = IRB.CreateAdd(MapDist, Distance); //在指定位置 添加distance距离; 编译的时候,距离已经写入了执行程序中
					IRB.CreateStore(IncrDist, MapDistPtr)->setMetadata(
							M.getMDKindID("nosanitize"), MDNode::get(C, None)); //保存

					/* Increase count at to shm[MAPSIZE + (4 or 8)] */

					Value *MapCntPtr = IRB.CreateGEP(MapPtr, MapCntLoc);
#ifdef LLVM_OLD_DEBUG_API
					LoadInst *MapCnt = IRB.CreateLoad(MapCntPtr);
					MapCnt->mutateType(LargestType);
#else
					LoadInst *MapCnt = IRB.CreateLoad(LargestType, MapCntPtr);
#endif
					MapCnt->setMetadata(M.getMDKindID("nosanitize"),
							MDNode::get(C, None));
					Value *IncrCnt = IRB.CreateAdd(MapCnt,
							ConstantInt::get(LargestType, 1)); //添加的距离数量加1,方便后面求平均值
					IRB.CreateStore(IncrCnt, MapCntPtr)->setMetadata(
							M.getMDKindID("nosanitize"), MDNode::get(C, None));

				}

				inst_blocks++;

			}
		}
	}

	/* Say something nice. */

	if (!is_aflgo_preprocessing && !be_quiet) {

		if (!inst_blocks)
			WARNF("No instrumentation targets found.");
		else
			OKF(
					"Instrumented %u locations (%s mode, ratio %u%%, dist. ratio %u%%).",
					inst_blocks,
					getenv("AFL_HARDEN") ?
							"hardened" :
							((getenv("AFL_USE_ASAN") || getenv("AFL_USE_MSAN")) ?
									"ASAN/MSAN" : "non-hardened"), inst_ratio,
					dinst_ratio);

	}

	return true;

}

//这个是注册函数
static void registerAFLPass(const PassManagerBuilder &,
		legacy::PassManagerBase &PM) {

	PM.add(new AFLCoverage());  //在这里注册了 pass

}

//自动加载 RegisterAFLPass 这个名字无所谓
static RegisterStandardPasses RegisterAFLPass(
		PassManagerBuilder::EP_OptimizerLast, registerAFLPass); //EP_OptimizerLast 在所有都跑完之后加pass,增加pass的点

static RegisterStandardPasses RegisterAFLPass0(
		PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass); //EP_EnabledOnOptLevel0 这个pass不会被 -O0优化点
