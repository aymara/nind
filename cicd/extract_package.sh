#!/bin/bash
set -u
set -e

if [ "$#" -ne 3 ]; then
 echo "Expect 3 arguments : <dockerImage> <packageDir> <outputEnvFile>"
 exit 1;
fi

dockerImage=$1
packageDir=$2
GITHUB_OUTPUT=$3

id=$(docker run -d "$dockerImage" sleep infinity)
echo -n "export fullname=$packageDir/" > ./envFile
docker exec $id ls $packageDir >> ./envFile
echo "export filename=\$(basename -- \$fullname)" >> ./envFile
echo "export trimmed=\$(basename \$filename .deb)" >> ./envFile
echo "tmp=\${trimmed#*-}" >> ./envFile
echo "export version=\${tmp%-*}" >> ./envFile
echo "filename=$(. envFile && echo $filename)" >> $GITHUB_OUTPUT
echo "version=$(. envFile && echo $version)"  >> $GITHUB_OUTPUT
. ./envFile && echo "filename=$filename , version=$version"
echo "Extract file from docker image"
. ./envFile && docker cp $id:$fullname - > ./$filename
docker stop $id
docker rm -v $id
echo "Finaly, the package should be present in local folder : "
if [ -f "./$filename" ]; then echo "!! yes : ./$filename" ; else echo "!! no"; exit 1; fi




