plugins {
    `java-library`
    `maven-publish`
}

group = "com.zombieos"
version = "1.0.0.0"

repositories {
    mavenCentral()
}

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(17)
    }
    withSourcesJar()
    withJavadocJar()
}

publishing {
    publications {
        create<MavenPublication>("mavenJava") {
            from(components["java"])
            artifactId = "zsharp"
        }
    }
    val githubRepository = System.getenv("GITHUB_REPOSITORY")
    if (!githubRepository.isNullOrBlank()) {
        repositories {
            maven {
                name = "GitHubPackages"
                url = uri(
                    "https://maven.pkg.github.com/${githubRepository.lowercase()}"
                )
                credentials {
                    username = System.getenv("GITHUB_ACTOR")
                    password = System.getenv("GITHUB_TOKEN")
                }
            }
        }
    }
}

tasks.withType<JavaCompile>().configureEach {
    options.encoding = "UTF-8"
}
