pipeline {
  agent any
  stages {
stage('Build, Test, and Package') {
      parallel {
        stage('SGX1FLC Package Debug') {
          agent {
            node {
              label 'hardware'
          }

          }
          steps {
withCredentials([file(credentialsId: 'oeciteam_rsa', variable: 'oeciteam-rsa')
                 ]) {
            sh "cp \$oeciteam-rsa ~/.ssh/id_rsa"
           }
        }
      }
    }
  }
}
}
