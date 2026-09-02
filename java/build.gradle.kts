plugins {
    `java-library`
    `maven-publish`
}

group = "com.zombieos"
version = "1.0.2.0"

repositories {
    mavenCentral()
}

dependencies {
    testImplementation("org.junit.jupiter:junit-jupiter:5.11.4")
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
            pom {
                name.set("Z# Java Integration")
                description.set("Run the Z# compiler and virtual machine from Java projects.")
                url.set("https://github.com/ZombieOS/ZSharp")
                licenses {
                    license {
                        name.set("Apache License, Version 2.0")
                        url.set("https://www.apache.org/licenses/LICENSE-2.0.txt")
                        distribution.set("repo")
                    }
                }
                developers {
                    developer {
                        name.set("Jack Johnson")
                        email.set("jack.johnson@zombieos.com")
                        organization.set("ZombieOS")
                        organizationUrl.set("https://zombieos.com")
                    }
                }
                scm {
                    connection.set("scm:git:https://github.com/ZombieOS/ZSharp.git")
                    developerConnection.set("scm:git:ssh://git@github.com/ZombieOS/ZSharp.git")
                    url.set("https://github.com/ZombieOS/ZSharp")
                }
            }
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

tasks.test {
    useJUnitPlatform()
}

tasks.withType<Jar>().configureEach {
    from(rootProject.file("../LICENSE")) {
        into("META-INF")
    }
}
