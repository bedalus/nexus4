#!/bin/bash
#
# patch.list to contain a list of commit hashes available in the current repo, either from fetched remotes, or other branches.
# I'm just using this to merge in only the used stuff from linux 3.4.y
# It works by checking to see if a cherry-pick will apply, then converts it into a patch to check if the .c files changed have
# a corresponding .o file otherwise there's no point applying it. 
# This obviously requires the kernel to be already built before starting. 
#
# An original/unusual script by David Savage (bedalus@gmail.com)

while read line           
do           
COMMITHASH=$line
echo "hash is $COMMITHASH"

# here we go...

git cherry-pick $COMMITHASH --exit-code
if [ "$?" = "0" ]; then
	echo "successful commit - converting to patch"
	git diff HEAD^ HEAD > 01.patch
	git reset --hard HEAD^
	git apply 01.patch
	git status | grep -i "\." | grep -i "Modified" | grep -i "\.c" | sed 's/#	modified:   //' | sed 's/\.c/.o/'> 01.data

	PATCH=true
	while read line           
	do           
		if [ ! -e $line ]; then
			echo "No such file : $line"
			echo "Nothing to do"
			PATCH=false
			break
		fi
	done <01.data

	git apply -R 01.patch

	if $PATCH; then 
		git cherry-pick $COMMITHASH > /dev/null
	fi

else
	git cherry-pick --abort
fi

# go back to top, get next patch
done <patch.list


# it's all over
rm 01.data
exit 0

