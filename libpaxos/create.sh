CURDIR=`pwd`
DIR=`pwd`


find -L $DIR/paxos $DIR/evpaxos $DIR/sample $DIR/unit $DIR/utils -name "*.hpp" -o -name "*.h" -o -name "*.cpp" -o -name "*.c" > cscope_my.files

cscope -bkq -i cscope_my.files  -f cscope_my.out

for i in  $DIR/paxos $DIR/evpaxos $DIR/sample $DIR/unit $DIR/utils
do 
	cp .vimrc	$i/
done


