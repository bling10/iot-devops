import com.cloudbees.plugins.credentials.CredentialsScope
import com.cloudbees.plugins.credentials.SystemCredentialsProvider
import com.cloudbees.plugins.credentials.domains.Domain
import hudson.util.Secret
import org.jenkinsci.plugins.plaincredentials.impl.StringCredentialsImpl

def tokenFile = new File('/run/secrets/dingtalk_token')
if (!tokenFile.isFile()) {
    println 'DingTalk credential bootstrap skipped: secret file is unavailable.'
    return
}

def token = tokenFile.getText('UTF-8').trim()
if (!token || token == 'CHANGE_ME') {
    println 'DingTalk credential bootstrap skipped: secret value is empty or unchanged.'
    return
}

def credentialId = 'dingtalk-robot-token'
def provider = SystemCredentialsProvider.getInstance()
def store = provider.getStore()
def domain = Domain.global()
def existing = provider.getCredentials().find { credential -> credential.id == credentialId }
def replacement = new StringCredentialsImpl(
    CredentialsScope.GLOBAL,
    credentialId,
    'DingTalk robot access token for OTA pipeline notifications',
    Secret.fromString(token)
)

if (existing) {
    store.updateCredentials(domain, existing, replacement)
    println "Updated Jenkins credential: ${credentialId}"
} else {
    store.addCredentials(domain, replacement)
    println "Created Jenkins credential: ${credentialId}"
}

provider.save()
