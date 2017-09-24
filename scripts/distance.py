#!/usr/bin/env python3
#coding=utf-8
import argparse
import networkx as nx
import re

#Regular expression to find callee
pattern = re.compile('@.*?\(')

#这个只是改名而已,形式
def node_name (name):
  if is_cg:
    return "\"{%s}\"" % name
  else:
    return "\"{%s:" % name

#################################
# Find the graph node for a name
#################################
def find_nodes (name):
  n_name = node_name (name)
  n_list = list (filter (lambda d: 'label' in d[1] and n_name in d[1]['label'], G.nodes(data=True)))
  if len (n_list) > 0:
    return n_list
  else:
    return []

##################################
# Calculate Distance
##################################
def distance (name):
  
  distance = -1
  for (n, _) in find_nodes (name):  # n is the name of the node in the Graph
    d = 0.0
    i = 0 # i is the num 
    if is_cg:
      for (t, _) in targets:
        if nx.has_path (G, n, t):
          shortest = nx.dijkstra_path_length (G, n, t)
          d += 1.0 / (1.0 + shortest)  #调和距离的分母计算
          i += 1
    else:
      #CFG图中所有call行,得到name,还没有找到CFG图中的点
      for t_name in bb_distance:
        di = 0.0
        ii = 0 #表示次数
        #这里会有多个点的情况吗,即CFG图中有多个点是相同的基本块
        for (t, _) in find_nodes(t_name):
          # t 是call在CFG图中对应的点
          #Check if path exists
          #当前CFG图中是否有路径
          if nx.has_path (G, n, t) :
            shortest = nx.dijkstra_path_length(G, n, t)
            # 1是常量,避免为0;10 * bb_distance[t_name] 表示call行(基本块)的距离;shortest 表示当前行和call行之间的距离
            di += 1.0 / (1.0 + 10 * bb_distance[t_name] + shortest) #计算调和距离的分母,1.0 + 10 * bb_distance[t_name] + shortest
            ii += 1
        if ii != 0:
          d += di / ii #可能会通过多个call行,取平均数
          i += 1

    if d != 0 and (distance == -1 or distance > i / d) :
      distance = i / d #CG图中:得到函数n和目标函数之间的调和距离; CFG图中,得到当前CFG图,所有点单元和目标基本块之间的距离

  if distance != -1:
    out.write (name)
    out.write (",")
    out.write (str (distance))
    out.write ("\n")

# Main function
if __name__ == '__main__':
  parser = argparse.ArgumentParser ()# 创建 ArgumentParser() 对象
  # add_argument 添加参数
  parser.add_argument ('-d', '--dot', type=str, required=True, help="Path to dot-file representing the graph.")
  parser.add_argument ('-t', '--targets', type=str, required=True, help="Path to file specifying Target nodes.")
  parser.add_argument ('-o', '--out', type=str, required=True, help="Path to output file containing distance for each node.")
  parser.add_argument ('-n', '--names', type=str, required=True, help="Path to file containing name for each node.")
  parser.add_argument ('-c', '--cg_distance', type=str, help="Path to file containing call graph distance.")
  parser.add_argument ('-s', '--cg_callsites', type=str, help="Path to file containing mapping between basic blocks and called functions.")
  #解析参数
  args = parser.parse_args ()

  print ("\nParsing %s .." % args.dot)
  G = nx.DiGraph(nx.drawing.nx_pydot.read_dot(args.dot)) #重构dot的图,这个应该是一致的 这个是 有向图
  print (nx.info(G))

  is_cg = 1 if "Name: Call graph" in nx.info(G) else 0
  print ("\nWorking in %s mode.." % ("CG" if is_cg else "CFG"))

  # Process as ControlFlowGraph
  caller = ""
  cg_distance = {} #CG图中的距离值,即编译过的函数和目标函数在CG图上的调和距离; 这个是list,key是函数名称,value是距离
  #当前CFG图中的所有call行,以及它的距离
  bb_distance = {} #BBcall的内容,所有call其他函数的行还是基本块? 这里要重新看一下,是行还是什么?  用来保存每个基本单元(行还是基本块)的距离
  if not is_cg :

    if args.cg_distance is None:
      print ("Specify file containing CG-level distance (-c).")
      exit(1)

    elif args.cg_callsites is None:
      print ("Specify file containing mapping between basic blocks and called functions (-s).")
      exit(1)

    else:

      caller = args.dot.split(".")
      caller = caller[len(caller)-2] #从CFG文件名中提取出函数名称,CFG中的基本单元是基本块
      print ("Loading cg_distance for function '%s'.." % caller)

      with open(args.cg_distance, 'r') as f:
        for l in f.readlines():
          s = l.strip().split(",")
          cg_distance[s[0]] = float(s[1])#保存对应的函数的距离

      with open(args.cg_callsites, 'r') as f:
        for l in f.readlines():
          s = l.strip().split(",")
          #判断当前CFG中是否有对应的call行(基本块) 行:函数名
          if len(find_nodes(s[0])) > 0:
            #s[1]是被call的目标,该目标函数是否有距离
            #这里只记录了call行的距离
            if s[1] in cg_distance:
              if s[0] in bb_distance:
                if bb_distance[s[0]] > cg_distance[s[1]]:
                  bb_distance[s[0]] = cg_distance[s[1]] #取最小距离值
              else:
                bb_distance[s[0]] = cg_distance[s[1]]#记录 当前行(基本块)和目标的距离和被call函数的距离一致 记录的是CFG图中基本块的距离

      print ("Adding target BBs (if any)..")
      #问题 这里 BBtareget中的内容和基本块之间的关系?
      #如果CFG图中有目标, 则对应目标的距离为0
      with open(args.targets, "r") as f: #CFG计算时的target来自于 BBtrgets,这个是代码中提取出来的, 文件名+行号
        for l in f.readlines ():
          s = l.strip().split("/");
          line = s[len(s) - 1] #get he line number of the targets
          #????判断CFG图中是否有目标, 这里CFG是基本块级别的,目标是target是什么级别的? 能够对应吗?
          nodes = find_nodes(line)  
          if len(nodes) > 0:
            bb_distance[line] = 0 #如果是目标本身,则距离为0
            print ("Added target BB!")

  # Process as CallGraph
  else:

    print ("Loading targets..")
    with open(args.targets, "r") as f:
      targets = [] #记录所有CG图中存在的target,每个单元是函数,是目标行所在的函数
      for line in f.readlines ():
        line = line.strip ()
        for target in find_nodes(line):
          targets.append (target)

    if (len (targets) == 0 and is_cg):
      print ("No targets available")
      exit(1)

  print ("Calculating distance..")
  with open(args.out, "w") as out:
    with open(args.names, "r") as f: #遍历CG图中所有的函数 ; 遍历所有编译过的 文件名:行号: (这个是基本单元)
      for line in f.readlines():#遍历所有编译过的函数
        line = line.strip()
        distance (line) # 第一种CG中line是所有函数名称;  CFG图中:计算所有编译过的基本单元(行号?基本块)和目标之间的距离