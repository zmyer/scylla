#!/bin/bash -e

if [ ! -e dist/ami/build_ami.sh ]; then
    echo "run build_ami.sh in top of scylla dir"
    exit 1
fi

print_usage() {
    echo "build_ami.sh --localrpm --repo [URL]"
    echo "  --localrpm  deploy locally built rpms"
    echo "  --repo  repository for both install and update, specify .repo/.list file URL"
    echo "  --repo-for-install  repository for install, specify .repo/.list file URL"
    echo "  --repo-for-update  repository for update, specify .repo/.list file URL"
    exit 1
}
LOCALRPM=0
while [ $# -gt 0 ]; do
    case "$1" in
        "--localrpm")
            LOCALRPM=1
            REPO=`./scripts/scylla_current_repo`
            INSTALL_ARGS="$INSTALL_ARGS --localrpm --repo $REPO"
            shift 1
            ;;
        "--repo")
            INSTALL_ARGS="$INSTALL_ARGS --repo $2"
            shift 2
            ;;
        "--repo-for-install")
            INSTALL_ARGS="$INSTALL_ARGS --repo-for-install $2"
            shift 2
            ;;
        "--repo-for-update")
            INSTALL_ARGS="$INSTALL_ARGS --repo-for-update $2"
            shift 2
            ;;
        *)
            print_usage
            ;;
    esac
done

. /etc/os-release
case "$ID" in
    "centos")
        AMI=ami-46bf8a51
        REGION=us-east-1
        SSH_USERNAME=centos
        ;;
    "ubuntu")
        AMI=ami-ff427095
        REGION=us-east-1
        SSH_USERNAME=ubuntu
        ;;
    *)
        echo "build_ami.sh does not supported this distribution."
        exit 1
        ;;
esac


if [ $LOCALRPM -eq 1 ]; then
    if [ "$ID" = "centos" ]; then
        rm -rf build/*
        sudo yum -y install git
        if [ ! -f dist/ami/files/scylla.x86_64.rpm ] || [ ! -f dist/ami/files/scylla-kernel-conf.x86_64.rpm ] || [ ! -f dist/ami/files/scylla-conf.x86_64.rpm ] || [ ! -f dist/ami/files/scylla-server.x86_64.rpm ] || [ ! -f dist/ami/files/scylla-debuginfo.x86_64.rpm ]; then
            dist/redhat/build_rpm.sh
            cp build/rpmbuild/RPMS/x86_64/scylla-`cat build/SCYLLA-VERSION-FILE`-`cat build/SCYLLA-RELEASE-FILE`.*.x86_64.rpm dist/ami/files/scylla.x86_64.rpm
            cp build/rpmbuild/RPMS/x86_64/scylla-kernel-conf-`cat build/SCYLLA-VERSION-FILE`-`cat build/SCYLLA-RELEASE-FILE`.*.x86_64.rpm dist/ami/files/scylla-kernel-conf.x86_64.rpm
            cp build/rpmbuild/RPMS/x86_64/scylla-conf-`cat build/SCYLLA-VERSION-FILE`-`cat build/SCYLLA-RELEASE-FILE`.*.x86_64.rpm dist/ami/files/scylla-conf.x86_64.rpm
            cp build/rpmbuild/RPMS/x86_64/scylla-server-`cat build/SCYLLA-VERSION-FILE`-`cat build/SCYLLA-RELEASE-FILE`.*.x86_64.rpm dist/ami/files/scylla-server.x86_64.rpm
            cp build/rpmbuild/RPMS/x86_64/scylla-debuginfo-`cat build/SCYLLA-VERSION-FILE`-`cat build/SCYLLA-RELEASE-FILE`.*.x86_64.rpm dist/ami/files/scylla-debuginfo.x86_64.rpm
        fi
        if [ ! -f dist/ami/files/scylla-jmx.noarch.rpm ]; then
            cd build
            git clone --depth 1 https://github.com/scylladb/scylla-jmx.git
            cd scylla-jmx
            sh -x -e dist/redhat/build_rpm.sh $*
            cd ../..
            cp build/scylla-jmx/build/rpmbuild/RPMS/noarch/scylla-jmx-`cat build/scylla-jmx/build/SCYLLA-VERSION-FILE`-`cat build/scylla-jmx/build/SCYLLA-RELEASE-FILE`.*.noarch.rpm dist/ami/files/scylla-jmx.noarch.rpm
        fi
        if [ ! -f dist/ami/files/scylla-tools.noarch.rpm ]; then
            cd build
            git clone --depth 1 https://github.com/scylladb/scylla-tools-java.git
            cd scylla-tools-java
            sh -x -e dist/redhat/build_rpm.sh
            cd ../..
            cp build/scylla-tools-java/build/rpmbuild/RPMS/noarch/scylla-tools-`cat build/scylla-tools-java/build/SCYLLA-VERSION-FILE`-`cat build/scylla-tools-java/build/SCYLLA-RELEASE-FILE`.*.noarch.rpm dist/ami/files/scylla-tools.noarch.rpm
        fi
    else
        sudo apt-get install -y git
        if [ ! -f dist/ami/files/scylla-server_amd64.deb ]; then
            if [ ! -f ../scylla-server_`cat build/SCYLLA-VERSION-FILE | sed 's/\.rc/~rc/'`-`cat build/SCYLLA-RELEASE-FILE`-ubuntu1_amd64.deb ]; then
                echo "Build .deb before running build_ami.sh"
                exit 1
            fi
            cp ../scylla_`cat build/SCYLLA-VERSION-FILE | sed 's/\.rc/~rc/'`-`cat build/SCYLLA-RELEASE-FILE`-ubuntu1_amd64.deb dist/ami/files/scylla_amd64.deb
            cp ../scylla-kernel-conf_`cat build/SCYLLA-VERSION-FILE | sed 's/\.rc/~rc/'`-`cat build/SCYLLA-RELEASE-FILE`-ubuntu1_amd64.deb dist/ami/files/scylla-kernel-conf_amd64.deb
            cp ../scylla-conf_`cat build/SCYLLA-VERSION-FILE | sed 's/\.rc/~rc/'`-`cat build/SCYLLA-RELEASE-FILE`-ubuntu1_amd64.deb dist/ami/files/scylla-conf_amd64.deb
            cp ../scylla-server_`cat build/SCYLLA-VERSION-FILE | sed 's/\.rc/~rc/'`-`cat build/SCYLLA-RELEASE-FILE`-ubuntu1_amd64.deb dist/ami/files/scylla-server_amd64.deb
            cp ../scylla-server-dbg_`cat build/SCYLLA-VERSION-FILE | sed 's/\.rc/~rc/'`-`cat build/SCYLLA-RELEASE-FILE`-ubuntu1_amd64.deb dist/ami/files/scylla-server-dbg_amd64.deb
        fi
        if [ ! -f dist/ami/files/scylla-jmx_all.deb ]; then
            cd build
            git clone --depth 1 https://github.com/scylladb/scylla-jmx.git
            cd scylla-jmx
            sh -x -e dist/debian/build_deb.sh $*
            cd ../..
            cp build/scylla-jmx_`cat build/scylla-jmx/build/SCYLLA-VERSION-FILE | sed 's/\.rc/~rc/'`-`cat build/scylla-jmx/build/SCYLLA-RELEASE-FILE`-ubuntu1_all.deb dist/ami/files/scylla-jmx_all.deb
        fi
        if [ ! -f dist/ami/files/scylla-tools_all.deb ]; then
            cd build
            git clone --depth 1 https://github.com/scylladb/scylla-tools-java.git
            cd scylla-tools-java
            sh -x -e dist/debian/build_deb.sh $*
            cd ../..
            cp build/scylla-tools_`cat build/scylla-tools-java/build/SCYLLA-VERSION-FILE | sed 's/\.rc/~rc/'`-`cat build/scylla-tools-java/build/SCYLLA-RELEASE-FILE`-ubuntu1_all.deb dist/ami/files/scylla-tools_all.deb
        fi
    fi
fi

cd dist/ami

if [ ! -f variables.json ]; then
    echo "create variables.json before start building AMI"
    exit 1
fi

if [ ! -d packer ]; then
    wget -nv https://releases.hashicorp.com/packer/0.8.6/packer_0.8.6_linux_amd64.zip
    mkdir packer
    cd packer
    unzip -x ../packer_0.8.6_linux_amd64.zip
    cd -
fi

packer/packer build -var-file=variables.json -var install_args="$INSTALL_ARGS" -var region="$REGION" -var source_ami="$AMI" -var ssh_username="$SSH_USERNAME" scylla.json
